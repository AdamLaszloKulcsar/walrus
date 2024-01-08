/*
 * Copyright (c) 2022-present Samsung Electronics Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 e*
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "Walrus.h"

#include "parser/WASMParser.h"
#include "interpreter/ByteCode.h"
#include "runtime/Store.h"
#include "runtime/Module.h"

#include "runtime/Value.h"
#include "wabt/walrus/binary-reader-walrus.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>

namespace wabt {

enum class WASMOpcode : size_t {
#define WABT_OPCODE(rtype, type1, type2, type3, memSize, prefix, code, name, \
                    text, decomp)                                            \
    name##Opcode,
#include "wabt/opcode.def"
#undef WABT_OPCODE
    OpcodeKindEnd,
};

struct WASMCodeInfo {
    enum CodeType { ___,
                    I32,
                    I64,
                    F32,
                    F64,
                    V128 };
    WASMOpcode m_code;
    CodeType m_resultType;
    CodeType m_paramTypes[3];
    const char *m_name;

    size_t stackShrinkSize() const
    {
        ASSERT(m_code != WASMOpcode::OpcodeKindEnd);
        return codeTypeToMemorySize(m_paramTypes[0]) + codeTypeToMemorySize(m_paramTypes[1]) + codeTypeToMemorySize(m_paramTypes[2]);
    }

    size_t stackGrowSize() const
    {
        ASSERT(m_code != WASMOpcode::OpcodeKindEnd);
        return codeTypeToMemorySize(m_resultType);
    }

    static size_t codeTypeToMemorySize(CodeType tp)
    {
        switch (tp) {
        case I32:
            return Walrus::stackAllocatedSize<int32_t>();
        case F32:
            return Walrus::stackAllocatedSize<float>();
        case I64:
            return Walrus::stackAllocatedSize<int64_t>();
        case F64:
            return Walrus::stackAllocatedSize<double>();
        case V128:
            return 16;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return 0;
        }
    }

    static Walrus::Value::Type codeTypeToValueType(CodeType tp)
    {
        switch (tp) {
        case I32:
            return Walrus::Value::Type::I32;
        case F32:
            return Walrus::Value::Type::F32;
        case I64:
            return Walrus::Value::Type::I64;
        case F64:
            return Walrus::Value::Type::F64;
        case V128:
            return Walrus::Value::Type::V128;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return Walrus::Value::Type::Void;
        }
    }
};

WASMCodeInfo g_wasmCodeInfo[static_cast<size_t>(WASMOpcode::OpcodeKindEnd)] = {
#define WABT_OPCODE(rtype, type1, type2, type3, memSize, prefix, code, name, \
                    text, decomp)                                            \
    { WASMOpcode::name##Opcode,                                              \
      WASMCodeInfo::rtype,                                                   \
      { WASMCodeInfo::type1, WASMCodeInfo::type2, WASMCodeInfo::type3 },     \
      text },
#include "wabt/opcode.def"
#undef WABT_OPCODE
};

static Walrus::Value::Type toValueKind(Type type)
{
    switch (type) {
    case Type::I32:
        return Walrus::Value::Type::I32;
    case Type::I64:
        return Walrus::Value::Type::I64;
    case Type::F32:
        return Walrus::Value::Type::F32;
    case Type::F64:
        return Walrus::Value::Type::F64;
    case Type::V128:
        return Walrus::Value::Type::V128;
    case Type::FuncRef:
        return Walrus::Value::Type::FuncRef;
    case Type::ExternRef:
        return Walrus::Value::Type::ExternRef;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static Walrus::SegmentMode toSegmentMode(uint8_t flags)
{
    enum SegmentFlags : uint8_t {
        SegFlagsNone = 0,
        SegPassive = 1, // bit 0: Is passive
        SegExplicitIndex = 2, // bit 1: Has explict index (Implies table 0 if absent)
        SegDeclared = 3, // Only used for declared segments
        SegUseElemExprs = 4, // bit 2: Is elemexpr (Or else index sequence)
        SegFlagMax = (SegUseElemExprs << 1) - 1, // All bits set.
    };

    if ((flags & SegDeclared) == SegDeclared) {
        return Walrus::SegmentMode::Declared;
    } else if ((flags & SegPassive) == SegPassive) {
        return Walrus::SegmentMode::Passive;
    } else {
        return Walrus::SegmentMode::Active;
    }
}

class WASMBinaryReader : public wabt::WASMBinaryReaderDelegate {
private:
    struct stackElement {
        stackElement *prev;
        stackElement *next;
        Walrus::ByteCodeStackOffset pos;
        size_t idx;
    };

    struct variableRangeInfo {
    public:
        variableRangeInfo(Walrus::ByteCodeStackOffset originalPosition, Walrus::Value::Type type)
            : originalPosition(originalPosition)
            , needsInit(false)
            , type(type)
            , start(SIZE_MAX)
            , end(0)
            , pos(0)
            , assignedPosition(nullptr)
        {
        }

        variableRangeInfo()
            : originalPosition(0)
            , needsInit(false)
            , type(Walrus::Value::Void)
            , start(0)
            , end(0)
            , pos(0)
            , assignedPosition(nullptr)
        {
        }

        Walrus::ByteCodeStackOffset originalPosition;
        bool needsInit;
        Walrus::Value::Type type;
        size_t start;
        size_t end;
        Walrus::ByteCodeStackOffset pos;
        std::vector<size_t> sets;
        std::vector<size_t> gets;
        stackElement *assignedPosition;
    };

    struct VMStackInfo {
    public:
        VMStackInfo(WASMBinaryReader &reader, Walrus::Value::Type valueType, size_t position, size_t nonOptimizedPosition, size_t localIndex)
            : m_reader(reader)
            , m_valueType(valueType)
            , m_position(position)
            , m_nonOptimizedPosition(nonOptimizedPosition)
            , m_localIndex(localIndex)
        {
        }

        VMStackInfo(const VMStackInfo &src)
            : m_reader(src.m_reader)
            , m_valueType(src.m_valueType)
            , m_position(src.m_position)
            , m_nonOptimizedPosition(src.m_nonOptimizedPosition)
            , m_localIndex(src.m_localIndex)
        {
        }

        ~VMStackInfo()
        {
        }

        const VMStackInfo &operator=(const VMStackInfo &src)
        {
            m_valueType = src.m_valueType;
            m_position = src.m_position;
            m_nonOptimizedPosition = src.m_nonOptimizedPosition;
            m_localIndex = src.m_localIndex;
            return *this;
        }

        bool hasValidLocalIndex() const
        {
            return m_localIndex != std::numeric_limits<size_t>::max();
        }

        void clearLocalIndex()
        {
            m_localIndex = std::numeric_limits<size_t>::max();
        }

        size_t position() const
        {
            return m_position;
        }

        void setPosition(size_t position)
        {
            m_position = position;
        }

        Walrus::Value::Type valueType() const
        {
            return m_valueType;
        }

        size_t stackAllocatedSize() const
        {
            return Walrus::valueStackAllocatedSize(m_valueType);
        }

        size_t nonOptimizedPosition() const
        {
            return m_nonOptimizedPosition;
        }

        size_t localIndex() const
        {
            return m_localIndex;
        }

    private:
        WASMBinaryReader &m_reader;
        Walrus::Value::Type m_valueType;
        size_t m_position; // effective position (local values will have different position)
        size_t m_nonOptimizedPosition; // non-optimized position (same with m_functionStackSizeSoFar)
        size_t m_localIndex;
    };

    struct BlockInfo {
        enum BlockType {
            IfElse,
            Loop,
            Block,
            TryCatch,
        };
        BlockType m_blockType;
        Type m_returnValueType;
        size_t m_position;
        std::vector<VMStackInfo> m_vmStack;
        uint32_t m_functionStackSizeSoFar;
        bool m_shouldRestoreVMStackAtEnd;
        bool m_byteCodeGenerationStopped;
        bool m_seenBranch;

        static_assert(sizeof(Walrus::JumpIfTrue) == sizeof(Walrus::JumpIfFalse), "");
        struct JumpToEndBrInfo {
            enum JumpToEndType {
                IsJump,
                IsJumpIf,
                IsBrTable,
            };

            JumpToEndType m_type;
            size_t m_position;
        };

        std::vector<JumpToEndBrInfo> m_jumpToEndBrInfo;

        BlockInfo(BlockType type, Type returnValueType, WASMBinaryReader &binaryReader)
            : m_blockType(type)
            , m_returnValueType(returnValueType)
            , m_position(0)
            , m_functionStackSizeSoFar(binaryReader.m_functionStackSizeSoFar)
            , m_shouldRestoreVMStackAtEnd((m_returnValueType.IsIndex() && binaryReader.m_result.m_functionTypes[m_returnValueType]->result().size())
                                          || m_returnValueType != Type::Void)
            , m_byteCodeGenerationStopped(false)
            , m_seenBranch(false)
        {
            if (returnValueType.IsIndex() && binaryReader.m_result.m_functionTypes[returnValueType]->param().size()) {
                // record parameter positions
                auto &param = binaryReader.m_result.m_functionTypes[returnValueType]->param();
                auto endIter = binaryReader.m_vmStack.rbegin() + param.size();
                auto iter = binaryReader.m_vmStack.rbegin();
                while (iter != endIter) {
                    if (iter->position() != iter->nonOptimizedPosition()) {
                        binaryReader.generateMoveCodeIfNeeds(iter->position(), iter->nonOptimizedPosition(), iter->valueType());
                        iter->setPosition(iter->nonOptimizedPosition());
                        if (binaryReader.m_preprocessData.m_inPreprocess && iter->hasValidLocalIndex()) {
                            size_t pos = *binaryReader.m_readerOffsetPointer;
                            auto localVariableIter = binaryReader.m_preprocessData
                                                         .m_localVariableInfo[iter->localIndex()]
                                                         .m_usageInfo.rbegin();
                            while (true) {
                                ASSERT(localVariableIter != binaryReader.m_preprocessData.m_localVariableInfo[iter->localIndex()].m_usageInfo.rend());
                                if (localVariableIter->m_endPosition == std::numeric_limits<size_t>::max()) {
                                    localVariableIter->m_endPosition = *binaryReader.m_readerOffsetPointer;
                                    break;
                                }
                                localVariableIter++;
                            }
                        }
                        iter->clearLocalIndex();
                    }
                    iter++;
                }
            }

            m_vmStack = binaryReader.m_vmStack;
            m_position = binaryReader.m_currentFunction->currentByteCodeSize();
        }
    };

    size_t *m_readerOffsetPointer;
    const uint8_t *m_readerDataPointer;
    size_t m_codeEndOffset;

    struct PreprocessData {
        struct LocalVariableInfo {
            bool m_needsExplicitInitOnStartup;
            LocalVariableInfo()
                : m_needsExplicitInitOnStartup(false)
            {
            }

            struct UsageInfo {
                size_t m_startPosition;
                size_t m_endPosition;
                size_t m_pushCount;
                bool m_hasWriteUsage;

                UsageInfo(size_t startPosition, size_t pushCount)
                    : m_startPosition(startPosition)
                    , m_endPosition(std::numeric_limits<size_t>::max())
                    , m_pushCount(pushCount)
                    , m_hasWriteUsage(false)
                {
                }
            };
            std::vector<size_t> m_definitelyWritePlaces;
            std::vector<size_t> m_writePlacesBetweenBranches;
            std::vector<UsageInfo> m_usageInfo;
        };

        PreprocessData(WASMBinaryReader &reader)
            : m_inPreprocess(false)
            , m_reader(reader)
        {
        }

        void clear()
        {
            m_localVariableInfo.clear();
            m_localVariableInfo.resize(m_reader.m_localInfo.size());
            m_constantData.clear();
        }

        void seenBranch()
        {
            if (m_inPreprocess) {
                if (m_reader.m_blockInfo.size()) {
                    m_reader.m_blockInfo.back().m_seenBranch = true;
                }
                for (auto &info : m_localVariableInfo) {
                    info.m_writePlacesBetweenBranches.clear();
                }
            }
        }

        void addLocalVariableUsage(size_t localIndex)
        {
            if (m_inPreprocess) {
                size_t pushCount = 0;
                size_t pos = *m_reader.m_readerOffsetPointer;
                for (const auto &stack : m_reader.m_vmStack) {
                    if (stack.localIndex() == localIndex) {
                        pushCount++;
                    }
                }
                m_localVariableInfo[localIndex].m_usageInfo.push_back(LocalVariableInfo::UsageInfo(pos, pushCount));
                if (!m_localVariableInfo[localIndex].m_needsExplicitInitOnStartup) {
                    if (!m_localVariableInfo[localIndex].m_writePlacesBetweenBranches.size()) {
                        bool writeFound = false;
                        const auto &definitelyWritePlaces = m_localVariableInfo[localIndex].m_definitelyWritePlaces;
                        for (size_t i = 0; i < definitelyWritePlaces.size(); i++) {
                            if (definitelyWritePlaces[i] < pos) {
                                writeFound = true;
                                break;
                            }
                        }
                        if (!writeFound) {
                            m_localVariableInfo[localIndex].m_needsExplicitInitOnStartup = true;
                        }
                    }
                }
            }
        }

        void addLocalVariableWrite(Index localIndex)
        {
            if (m_inPreprocess) {
                size_t pos = *m_reader.m_readerOffsetPointer;
                auto iter = m_localVariableInfo[localIndex].m_usageInfo.begin();
                while (iter != m_localVariableInfo[localIndex].m_usageInfo.end()) {
                    if (iter->m_startPosition <= pos && pos <= iter->m_endPosition) {
                        iter->m_hasWriteUsage = true;
                    }
                    iter++;
                }

                bool isDefinitelyWritePlaces = true;
                auto blockIter = m_reader.m_blockInfo.rbegin();
                while (blockIter != m_reader.m_blockInfo.rend()) {
                    if (blockIter->m_seenBranch) {
                        isDefinitelyWritePlaces = false;
                        break;
                    }
                    blockIter++;
                }

                if (isDefinitelyWritePlaces) {
                    m_localVariableInfo[localIndex].m_definitelyWritePlaces.push_back(pos);
                }

                m_localVariableInfo[localIndex].m_writePlacesBetweenBranches.push_back(pos);
            }
        }

        void addConstantData(const Walrus::Value &v)
        {
            if (m_inPreprocess) {
                bool found = false;
                for (size_t i = 0; i < m_constantData.size(); i++) {
                    if (m_constantData[i].first == v) {
                        m_constantData[i].second++;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    m_constantData.push_back(std::make_pair(v, 1));
                }

#ifndef WALRUS_ASSIGN_CONSTANT_ON_STACK_MAX_COUNT
#define WALRUS_ASSIGN_CONSTANT_ON_STACK_MAX_COUNT 6
#endif
                constexpr size_t maxConstantData = WALRUS_ASSIGN_CONSTANT_ON_STACK_MAX_COUNT;
                if (m_constantData.size() > maxConstantData) {
                    organizeConstantData();
                    m_constantData.erase(m_constantData.end() - maxConstantData / 4, m_constantData.end());
                }
            }
        }

        void organizeConstantData()
        {
            std::sort(m_constantData.begin(), m_constantData.end(),
                      [](const std::pair<Walrus::Value, size_t> &a, const std::pair<Walrus::Value, size_t> &b) -> bool {
                          return a.second > b.second;
                      });
        }

        void organizeData()
        {
            organizeConstantData();
        }

        bool m_inPreprocess;
        WASMBinaryReader &m_reader;
        std::vector<LocalVariableInfo> m_localVariableInfo;
        // <ConstantValue, reference count or position>
        std::vector<std::pair<Walrus::Value, size_t>> m_constantData;
    };

    bool m_inInitExpr;
    Walrus::ModuleFunction *m_currentFunction;
    Walrus::FunctionType *m_currentFunctionType;
    uint16_t m_initialFunctionStackSize;
    uint16_t m_functionStackSizeSoFar;

    std::vector<VMStackInfo> m_vmStack;
    std::vector<BlockInfo> m_blockInfo;
    struct CatchInfo {
        size_t m_tryCatchBlockDepth;
        size_t m_tryStart;
        size_t m_tryEnd;
        size_t m_catchStart;
        uint32_t m_tagIndex;
    };
    std::vector<CatchInfo> m_catchInfo;
    struct LocalInfo {
        Walrus::Value::Type m_valueType;
        size_t m_position;
        LocalInfo(Walrus::Value::Type type, size_t position)
            : m_valueType(type)
            , m_position(position)
        {
        }
    };
    std::vector<LocalInfo> m_localInfo;

    Walrus::Vector<uint8_t, std::allocator<uint8_t>> m_memoryInitData;

    uint32_t m_elementTableIndex;
    Walrus::Optional<Walrus::ModuleFunction *> m_elementOffsetFunction;
    Walrus::Vector<Walrus::ModuleFunction *> m_elementExprFunctions;
    Walrus::SegmentMode m_segmentMode;

    Walrus::WASMParsingResult m_result;

    PreprocessData m_preprocessData;

    std::vector<std::pair<Walrus::Value::Type, size_t>> m_stackValues;

    // i32.eqz and JumpIf can be unified in some cases
    static const size_t s_noI32Eqz = SIZE_MAX - sizeof(Walrus::I32Eqz);
    size_t m_lastI32EqzPos;

    virtual void OnSetOffsetAddress(size_t *ptr) override
    {
        m_readerOffsetPointer = ptr;
    }

    virtual void OnSetDataAddress(const uint8_t *data) override
    {
        m_readerDataPointer = data;
    }

    size_t pushVMStack(Walrus::Value::Type type)
    {
        auto pos = m_functionStackSizeSoFar;
        pushVMStack(type, pos);
        return pos;
    }

    void pushVMStack(Walrus::Value::Type type, size_t pos, size_t localIndex = std::numeric_limits<size_t>::max())
    {
        if (localIndex != std::numeric_limits<size_t>::max()) {
            m_preprocessData.addLocalVariableUsage(localIndex);
        }

        m_vmStack.push_back(VMStackInfo(*this, type, pos, m_functionStackSizeSoFar, localIndex));
        size_t allocSize = Walrus::valueStackAllocatedSize(type);
        // FIXME too many stack usage. we could not support this(yet)
        ASSERT(m_functionStackSizeSoFar + allocSize <= std::numeric_limits<Walrus::ByteCodeStackOffset>::max());

        if (localIndex == SIZE_MAX) {
            m_stackValues.push_back({ type, pos });
        }

        m_functionStackSizeSoFar += allocSize;
        m_currentFunction->m_requiredStackSize = std::max(
            m_currentFunction->m_requiredStackSize, m_functionStackSizeSoFar);
    }

    VMStackInfo popVMStackInfo()
    {
        // FIXME This error can occur during the parsing process because of invalid wasm instructions
        // e.g. a function with no end opcode
        ASSERT(m_vmStack.size() > 0);

        auto info = m_vmStack.back();
        m_functionStackSizeSoFar -= Walrus::valueStackAllocatedSize(info.valueType());
        m_vmStack.pop_back();

        if (m_preprocessData.m_inPreprocess) {
            if (info.hasValidLocalIndex()) {
                size_t pos = *m_readerOffsetPointer;
                auto iter = m_preprocessData.m_localVariableInfo[info.localIndex()].m_usageInfo.rbegin();
                while (true) {
                    ASSERT(iter != m_preprocessData.m_localVariableInfo[info.localIndex()].m_usageInfo.rend());
                    if (iter->m_endPosition == std::numeric_limits<size_t>::max()) {
                        iter->m_endPosition = *m_readerOffsetPointer;
                        break;
                    }
                    iter++;
                }
            }
        }

        return info;
    }

    VMStackInfo &peekVMStackInfo()
    {
        // FIXME This error can occur during the parsing process because of invalid wasm instructions
        // e.g. a function with no end opcode
        ASSERT(m_vmStack.size() > 0);
        return m_vmStack.back();
    }

    size_t popVMStack()
    {
        return popVMStackInfo().position();
    }

    size_t peekVMStack()
    {
        return peekVMStackInfo().position();
    }

    Walrus::Value::Type peekVMStackValueType()
    {
        return peekVMStackInfo().valueType();
    }

    void beginFunction(Walrus::ModuleFunction *mf, bool inInitExpr)
    {
        m_inInitExpr = inInitExpr;
        m_currentFunction = mf;
        m_currentFunctionType = mf->functionType();
        m_localInfo.clear();
        m_localInfo.reserve(m_currentFunctionType->param().size());
        size_t pos = 0;
        for (size_t i = 0; i < m_currentFunctionType->param().size(); i++) {
            m_localInfo.push_back(LocalInfo(m_currentFunctionType->param()[i], pos));
            pos += Walrus::valueStackAllocatedSize(m_localInfo[i].m_valueType);
        }
        m_initialFunctionStackSize = m_functionStackSizeSoFar = m_currentFunctionType->paramStackSize();
        m_currentFunction->m_requiredStackSize = std::max(
            m_currentFunction->m_requiredStackSize, m_functionStackSizeSoFar);
    }

    void endFunction()
    {
        optimizeLocals();

#if !defined(NDEBUG)
        if (getenv("DUMP_BYTECODE") && strlen(getenv("DUMP_BYTECODE"))) {
            m_currentFunction->dumpByteCode();
        }
        if (m_shouldContinueToGenerateByteCode) {
            for (size_t i = 0; i < m_currentFunctionType->result().size() && m_vmStack.size(); i++) {
                ASSERT(popVMStackInfo().valueType() == m_currentFunctionType->result()[m_currentFunctionType->result().size() - i - 1]);
            }
            ASSERT(m_vmStack.empty());
        }
#endif

        m_currentFunction = nullptr;
        m_currentFunctionType = nullptr;
        m_vmStack.clear();
        m_shouldContinueToGenerateByteCode = true;
    }

    template <typename CodeType>
    void pushByteCode(const CodeType &code, WASMOpcode opcode)
    {
        m_currentFunction->pushByteCode(code);
    }

    void pushByteCode(const Walrus::I32Eqz &code, WASMOpcode opcode)
    {
        m_lastI32EqzPos = m_currentFunction->currentByteCodeSize();
        m_currentFunction->pushByteCode(code);
    }

    inline bool canBeInverted(size_t stackPos)
    {
        /**
         * m_lastI32EqzPos + sizeof(Walrus::I32Eqz) == m_currentFunction->currentByteCodeSize()
         * checks if last byteCode is I32Eqz
         *
         * m_currentFunction->peekByteCode<Walrus::UnaryOperation>(m_lastI32EqzPos)->dstOffset() == stackPos
         * checks if the output of I32Eqz is the input of JumpIfTrue/JumpIfFalse
         */
        return (m_lastI32EqzPos + sizeof(Walrus::I32Eqz) == m_currentFunction->currentByteCodeSize())
            && (m_currentFunction->peekByteCode<Walrus::UnaryOperation>(m_lastI32EqzPos)->dstOffset() == stackPos);
    }

    Walrus::Optional<uint8_t> lookaheadUnsigned8(size_t offset = 0)
    {
        if (*m_readerOffsetPointer + offset < m_codeEndOffset) {
            return m_readerDataPointer[*m_readerOffsetPointer + offset];
        }
        return Walrus::Optional<uint8_t>();
    }

    std::pair<Walrus::Optional<uint32_t>, size_t> lookaheadUnsigned32(size_t offset = 0) // returns result value and data-length
    {
        if (*m_readerOffsetPointer + offset < m_codeEndOffset) {
            uint32_t out = 0;
            auto ptr = m_readerDataPointer + *m_readerOffsetPointer + offset;
            auto len = ReadU32Leb128(ptr, m_readerDataPointer + m_codeEndOffset, &out);
            return std::make_pair(out, len);
        }
        return std::make_pair(Walrus::Optional<uint32_t>(), 0);
    }

    std::pair<Walrus::Optional<uint32_t>, size_t> readAheadLocalGetIfExists() // return localIndex and code length if exists
    {
        Walrus::Optional<uint8_t> mayLoadGetCode = lookaheadUnsigned8();
        if (mayLoadGetCode.hasValue() && mayLoadGetCode.value() == 0x21) {
            auto r = lookaheadUnsigned32(1);
            if (r.first) {
                return std::make_pair(r.first, r.second + 1);
            }
        }

        return std::make_pair(Walrus::Optional<uint32_t>(), 0);
    }

public:
    WASMBinaryReader()
        : m_readerOffsetPointer(nullptr)
        , m_readerDataPointer(nullptr)
        , m_codeEndOffset(0)
        , m_inInitExpr(false)
        , m_currentFunction(nullptr)
        , m_currentFunctionType(nullptr)
        , m_initialFunctionStackSize(0)
        , m_functionStackSizeSoFar(0)
        , m_elementTableIndex(0)
        , m_segmentMode(Walrus::SegmentMode::None)
        , m_preprocessData(*this)
        , m_lastI32EqzPos(s_noI32Eqz)
    {
    }

    ~WASMBinaryReader()
    {
        // clear stack first! because vmStack refer localInfo
        m_vmStack.clear();
        m_localInfo.clear();

        m_result.clear();
    }

    // should be allocated on the stack
    static void *operator new(size_t) = delete;
    static void *operator new[](size_t) = delete;

    virtual void BeginModule(uint32_t version) override
    {
        m_result.m_version = version;
    }

    virtual void EndModule() override {}

    virtual void OnTypeCount(Index count) override
    {
        // TODO reserve vector if possible
    }

    virtual void OnFuncType(Index index,
                            Index paramCount,
                            Type *paramTypes,
                            Index resultCount,
                            Type *resultTypes) override
    {
        Walrus::ValueTypeVector *param = new Walrus::ValueTypeVector();
        param->reserve(paramCount);
        for (size_t i = 0; i < paramCount; i++) {
            param->push_back(toValueKind(paramTypes[i]));
        }
        Walrus::ValueTypeVector *result = new Walrus::ValueTypeVector();
        for (size_t i = 0; i < resultCount; i++) {
            result->push_back(toValueKind(resultTypes[i]));
        }
        ASSERT(index == m_result.m_functionTypes.size());
        m_result.m_functionTypes.push_back(new Walrus::FunctionType(param, result));
    }

    virtual void OnImportCount(Index count) override
    {
        m_result.m_imports.reserve(count);
    }

    virtual void OnImportFunc(Index importIndex,
                              std::string moduleName,
                              std::string fieldName,
                              Index funcIndex,
                              Index sigIndex) override
    {
        ASSERT(m_result.m_functions.size() == funcIndex);
        ASSERT(m_result.m_imports.size() == importIndex);
        Walrus::FunctionType *ft = m_result.m_functionTypes[sigIndex];
        m_result.m_functions.push_back(
            new Walrus::ModuleFunction(ft));
        m_result.m_imports.push_back(new Walrus::ImportType(
            Walrus::ImportType::Function,
            moduleName, fieldName, ft));
    }

    virtual void OnImportGlobal(Index importIndex, std::string moduleName, std::string fieldName, Index globalIndex, Type type, bool mutable_) override
    {
        ASSERT(globalIndex == m_result.m_globalTypes.size());
        ASSERT(m_result.m_imports.size() == importIndex);
        m_result.m_globalTypes.push_back(new Walrus::GlobalType(toValueKind(type), mutable_));
        m_result.m_imports.push_back(new Walrus::ImportType(
            Walrus::ImportType::Global,
            moduleName, fieldName, m_result.m_globalTypes[globalIndex]));
    }

    virtual void OnImportTable(Index importIndex, std::string moduleName, std::string fieldName, Index tableIndex, Type type, size_t initialSize, size_t maximumSize) override
    {
        ASSERT(tableIndex == m_result.m_tableTypes.size());
        ASSERT(m_result.m_imports.size() == importIndex);
        ASSERT(type == Type::FuncRef || type == Type::ExternRef);

        m_result.m_tableTypes.push_back(new Walrus::TableType(type == Type::FuncRef ? Walrus::Value::Type::FuncRef : Walrus::Value::Type::ExternRef, initialSize, maximumSize));
        m_result.m_imports.push_back(new Walrus::ImportType(
            Walrus::ImportType::Table,
            moduleName, fieldName, m_result.m_tableTypes[tableIndex]));
    }

    virtual void OnImportMemory(Index importIndex, std::string moduleName, std::string fieldName, Index memoryIndex, size_t initialSize, size_t maximumSize, bool isShared) override
    {
        ASSERT(memoryIndex == m_result.m_memoryTypes.size());
        ASSERT(m_result.m_imports.size() == importIndex);
        m_result.m_memoryTypes.push_back(new Walrus::MemoryType(initialSize, maximumSize, isShared));
        m_result.m_imports.push_back(new Walrus::ImportType(
            Walrus::ImportType::Memory,
            moduleName, fieldName, m_result.m_memoryTypes[memoryIndex]));
    }

    virtual void OnImportTag(Index importIndex, std::string moduleName, std::string fieldName, Index tagIndex, Index sigIndex) override
    {
        ASSERT(tagIndex == m_result.m_tagTypes.size());
        ASSERT(m_result.m_imports.size() == importIndex);
        m_result.m_tagTypes.push_back(new Walrus::TagType(sigIndex));
        m_result.m_imports.push_back(new Walrus::ImportType(
            Walrus::ImportType::Tag,
            moduleName, fieldName, m_result.m_tagTypes[tagIndex]));
    }

    virtual void OnExportCount(Index count) override
    {
        m_result.m_exports.reserve(count);
    }

    virtual void OnExport(int kind, Index exportIndex, std::string name, Index itemIndex) override
    {
        ASSERT(m_result.m_exports.size() == exportIndex);
        m_result.m_exports.push_back(new Walrus::ExportType(static_cast<Walrus::ExportType::Type>(kind), name, itemIndex));
    }

    /* Table section */
    virtual void OnTableCount(Index count) override
    {
        m_result.m_tableTypes.reserve(count);
    }

    virtual void OnTable(Index index, Type type, size_t initialSize, size_t maximumSize) override
    {
        ASSERT(index == m_result.m_tableTypes.size());
        ASSERT(type == Type::FuncRef || type == Type::ExternRef);
        m_result.m_tableTypes.push_back(new Walrus::TableType(type == Type::FuncRef ? Walrus::Value::Type::FuncRef : Walrus::Value::Type::ExternRef, initialSize, maximumSize));
    }

    virtual void OnElemSegmentCount(Index count) override
    {
        m_result.m_elements.reserve(count);
    }

    virtual void BeginElemSegment(Index index, Index tableIndex, uint8_t flags) override
    {
        m_elementTableIndex = tableIndex;
        m_elementOffsetFunction = nullptr;
        m_segmentMode = toSegmentMode(flags);
    }

    virtual void BeginElemSegmentInitExpr(Index index) override
    {
        beginFunction(new Walrus::ModuleFunction(Walrus::Store::getDefaultFunctionType(Walrus::Value::I32)), true);
    }

    virtual void EndElemSegmentInitExpr(Index index) override
    {
        m_elementOffsetFunction = m_currentFunction;
        endFunction();
    }

    virtual void OnElemSegmentElemType(Index index, Type elemType) override
    {
    }

    virtual void OnElemSegmentElemExprCount(Index index, Index count) override
    {
        m_elementExprFunctions.reserve(count);
    }

    virtual void BeginElemExpr(Index elem_index, Index expr_index) override
    {
        beginFunction(new Walrus::ModuleFunction(Walrus::Store::getDefaultFunctionType(Walrus::Value::FuncRef)), true);
    }

    virtual void EndElemExpr(Index elem_index, Index expr_index) override
    {
        m_elementExprFunctions.push_back(m_currentFunction);
        endFunction();
    }

    virtual void EndElemSegment(Index index) override
    {
        ASSERT(m_result.m_elements.size() == index);
        if (m_elementOffsetFunction) {
            m_result.m_elements.push_back(new Walrus::Element(m_segmentMode, m_elementTableIndex, m_elementOffsetFunction.value(), std::move(m_elementExprFunctions)));
        } else {
            m_result.m_elements.push_back(new Walrus::Element(m_segmentMode, m_elementTableIndex, std::move(m_elementExprFunctions)));
        }

        m_elementOffsetFunction = nullptr;
        m_elementTableIndex = 0;
        m_segmentMode = Walrus::SegmentMode::None;
    }

    /* Memory section */
    virtual void OnMemoryCount(Index count) override
    {
        m_result.m_memoryTypes.reserve(count);
    }

    virtual void OnMemory(Index index, uint64_t initialSize, uint64_t maximumSize, bool isShared) override
    {
        ASSERT(index == m_result.m_memoryTypes.size());
        m_result.m_memoryTypes.push_back(new Walrus::MemoryType(initialSize, maximumSize, isShared));
    }

    virtual void OnDataSegmentCount(Index count) override
    {
        m_result.m_datas.reserve(count);
    }

    virtual void BeginDataSegment(Index index, Index memoryIndex, uint8_t flags) override
    {
        ASSERT(index == m_result.m_datas.size());
        beginFunction(new Walrus::ModuleFunction(Walrus::Store::getDefaultFunctionType(Walrus::Value::I32)), true);
    }

    virtual void BeginDataSegmentInitExpr(Index index) override
    {
    }

    virtual void EndDataSegmentInitExpr(Index index) override
    {
    }

    virtual void OnDataSegmentData(Index index, const void *data, Address size) override
    {
        m_memoryInitData.resizeWithUninitializedValues(size);
        memcpy(m_memoryInitData.data(), data, size);
    }

    virtual void EndDataSegment(Index index) override
    {
        ASSERT(index == m_result.m_datas.size());
        m_result.m_datas.push_back(new Walrus::Data(m_currentFunction, std::move(m_memoryInitData)));
        endFunction();
    }

    /* Function section */
    virtual void OnFunctionCount(Index count) override
    {
        m_result.m_functions.reserve(count);
    }

    virtual void OnFunction(Index index, Index sigIndex) override
    {
        ASSERT(m_currentFunction == nullptr);
        ASSERT(m_currentFunctionType == nullptr);
        ASSERT(m_result.m_functions.size() == index);
        m_result.m_functions.push_back(new Walrus::ModuleFunction(m_result.m_functionTypes[sigIndex]));
    }

    virtual void OnGlobalCount(Index count) override
    {
        m_result.m_globalTypes.reserve(count);
    }

    virtual void BeginGlobal(Index index, Type type, bool mutable_) override
    {
        ASSERT(m_result.m_globalTypes.size() == index);
        m_result.m_globalTypes.push_back(new Walrus::GlobalType(toValueKind(type), mutable_));
    }

    virtual void BeginGlobalInitExpr(Index index) override
    {
        auto ft = Walrus::Store::getDefaultFunctionType(m_result.m_globalTypes[index]->type());
        Walrus::ModuleFunction *mf = new Walrus::ModuleFunction(ft);
        m_result.m_globalTypes[index]->setFunction(mf);
        beginFunction(mf, true);
    }

    virtual void EndGlobalInitExpr(Index index) override
    {
        endFunction();
    }

    virtual void EndGlobal(Index index) override
    {
    }

    virtual void EndGlobalSection() override
    {
    }

    virtual void OnTagCount(Index count) override
    {
        m_result.m_tagTypes.reserve(count);
    }

    virtual void OnTagType(Index index, Index sigIndex) override
    {
        ASSERT(index == m_result.m_tagTypes.size());
        m_result.m_tagTypes.push_back(new Walrus::TagType(sigIndex));
    }

    virtual void OnStartFunction(Index funcIndex) override
    {
        m_result.m_seenStartAttribute = true;
        m_result.m_start = funcIndex;
    }

    virtual void BeginFunctionBody(Index index, Offset size) override
    {
        ASSERT(m_currentFunction == nullptr);
        beginFunction(m_result.m_functions[index], false);
    }

    virtual void OnLocalDeclCount(Index count) override
    {
        m_currentFunction->m_local.reserve(count);
        m_localInfo.reserve(count + m_currentFunctionType->param().size());
    }

    virtual void OnLocalDecl(Index decl_index, Index count, Type type) override
    {
        while (count) {
            auto wType = toValueKind(type);
            m_currentFunction->m_local.push_back(wType);
            m_localInfo.push_back(LocalInfo(wType, m_functionStackSizeSoFar));
            auto sz = Walrus::valueStackAllocatedSize(wType);
            m_initialFunctionStackSize += sz;
            m_functionStackSizeSoFar += sz;
            count--;
        }
        m_currentFunction->m_requiredStackSize = std::max(
            m_currentFunction->m_requiredStackSize, m_functionStackSizeSoFar);
    }

    // FIXME remove preprocess
    virtual void OnStartReadInstructions(Offset start, Offset end) override
    {
        ASSERT(start == *m_readerOffsetPointer);
        m_codeEndOffset = end;
    }

    virtual void OnStartPreprocess() override
    {
        m_preprocessData.m_inPreprocess = true;
        m_preprocessData.clear();
    }

    virtual void OnEndPreprocess() override
    {
        m_preprocessData.m_inPreprocess = false;
        m_skipValidationUntil = *m_readerOffsetPointer - 1;
        m_shouldContinueToGenerateByteCode = true;

        m_currentFunction->m_byteCode.clear();
        m_currentFunction->m_catchInfo.clear();
        m_blockInfo.clear();
        m_catchInfo.clear();

        m_vmStack.clear();

        m_preprocessData.organizeData();

        // set const variables position
        for (size_t i = 0; i < m_preprocessData.m_constantData.size(); i++) {
            auto constType = m_preprocessData.m_constantData[i].first.type();
            m_preprocessData.m_constantData[i].second = m_initialFunctionStackSize;
            m_initialFunctionStackSize += Walrus::valueStackAllocatedSize(constType);
        }

        m_functionStackSizeSoFar = m_initialFunctionStackSize;
        m_currentFunction->m_requiredStackSize = m_functionStackSizeSoFar;

        // init constant space
        for (size_t i = 0; i < m_preprocessData.m_constantData.size(); i++) {
            const auto &constValue = m_preprocessData.m_constantData[i].first;
            auto constType = m_preprocessData.m_constantData[i].first.type();
            auto constPos = m_preprocessData.m_constantData[i].second;
            size_t constSize = Walrus::valueSize(constType);

            uint8_t constantBuffer[16];
            constValue.writeToMemory(constantBuffer);
            if (constSize == 4) {
                pushByteCode(Walrus::Const32(constPos, *reinterpret_cast<uint32_t *>(constantBuffer)), WASMOpcode::I32ConstOpcode);
            } else if (constSize == 8) {
                pushByteCode(Walrus::Const64(constPos, *reinterpret_cast<uint64_t *>(constantBuffer)), WASMOpcode::I64ConstOpcode);
            } else {
                ASSERT(constSize == 16);
                pushByteCode(Walrus::Const128(constPos, constantBuffer), WASMOpcode::V128ConstOpcode);
            }
#if !defined(NDEBUG)
            m_currentFunction->m_constantDebugData.pushBack(m_preprocessData.m_constantData[i]);
#endif
        }
    }

    virtual void OnOpcode(uint32_t opcode) override
    {
    }

    uint16_t computeFunctionParameterOrResultOffsetCount(const Walrus::ValueTypeVector &types)
    {
        uint16_t result = 0;
        for (auto t : types) {
            result += valueFunctionCopyCount(t);
        }
        return result;
    }

    template <typename CodeType>
    void generateCallExpr(CodeType *code, uint16_t parameterCount, uint16_t resultCount,
                          Walrus::FunctionType *functionType)
    {
        size_t offsetIndex = 0;
        size_t siz = functionType->param().size();

        for (size_t i = 0; i < siz; i++) {
            ASSERT(peekVMStackValueType() == functionType->param()[siz - i - 1]);
            size_t sourcePos = popVMStack();
            auto type = functionType->param()[siz - i - 1];
            size_t s = Walrus::valueSize(type);
            size_t offsetSubCount = 0;
            size_t subIndexCount = valueFunctionCopyCount(type);
            for (size_t j = 0; j < s; j += sizeof(size_t)) {
                code->stackOffsets()[parameterCount - offsetIndex - subIndexCount + offsetSubCount++] = sourcePos + j;
            }
            offsetIndex += subIndexCount;
        }

        siz = functionType->result().size();
        for (size_t i = 0; i < siz; i++) {
            size_t dstPos = pushVMStack(functionType->result()[i]);
            size_t itemSize = Walrus::valueSize(functionType->result()[i]);
            for (size_t j = 0; j < itemSize; j += sizeof(size_t)) {
                code->stackOffsets()[offsetIndex++] = dstPos + j;
            }
        }
        ASSERT(offsetIndex == (code->parameterOffsetsSize() + code->resultOffsetsSize()));
    }

    virtual void OnCallExpr(uint32_t index) override
    {
        auto functionType = m_result.m_functions[index]->functionType();
        auto callPos = m_currentFunction->currentByteCodeSize();
        auto parameterCount = computeFunctionParameterOrResultOffsetCount(functionType->param());
        auto resultCount = computeFunctionParameterOrResultOffsetCount(functionType->result());
        pushByteCode(Walrus::Call(index, parameterCount, resultCount), WASMOpcode::CallOpcode);

        m_currentFunction->expandByteCode(Walrus::ByteCode::pointerAlignedSize(sizeof(Walrus::ByteCodeStackOffset) * (parameterCount + resultCount)));
        ASSERT(m_currentFunction->currentByteCodeSize() % sizeof(void *) == 0);
        auto code = m_currentFunction->peekByteCode<Walrus::Call>(callPos);

        generateCallExpr(code, parameterCount, resultCount, functionType);
    }

    virtual void OnCallIndirectExpr(Index sigIndex, Index tableIndex) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::I32);
        auto functionType = m_result.m_functionTypes[sigIndex];
        auto callPos = m_currentFunction->currentByteCodeSize();
        auto parameterCount = computeFunctionParameterOrResultOffsetCount(functionType->param());
        auto resultCount = computeFunctionParameterOrResultOffsetCount(functionType->result());
        pushByteCode(Walrus::CallIndirect(popVMStack(), tableIndex, functionType, parameterCount, resultCount),
                     WASMOpcode::CallIndirectOpcode);
        m_currentFunction->expandByteCode(Walrus::ByteCode::pointerAlignedSize(sizeof(Walrus::ByteCodeStackOffset) * (parameterCount + resultCount)));
        ASSERT(m_currentFunction->currentByteCodeSize() % sizeof(void *) == 0);

        auto code = m_currentFunction->peekByteCode<Walrus::CallIndirect>(callPos);
        generateCallExpr(code, parameterCount, resultCount, functionType);
    }

    bool processConstValue(const Walrus::Value &value)
    {
        if (!m_inInitExpr) {
            m_preprocessData.addConstantData(value);
            if (!m_preprocessData.m_inPreprocess) {
                for (size_t i = 0; i < m_preprocessData.m_constantData.size(); i++) {
                    if (m_preprocessData.m_constantData[i].first == value) {
                        pushVMStack(value.type(), m_preprocessData.m_constantData[i].second);
                        return true;
                    }
                }
            }
        }
        return false;
    }


    virtual void OnI32ConstExpr(uint32_t value) override
    {
        if (processConstValue(Walrus::Value(Walrus::Value::Type::I32, reinterpret_cast<uint8_t *>(&value)))) {
            return;
        }
        pushByteCode(Walrus::Const32(computeExprResultPosition(Walrus::Value::Type::I32), value), WASMOpcode::I32ConstOpcode);
    }

    virtual void OnI64ConstExpr(uint64_t value) override
    {
        if (processConstValue(Walrus::Value(Walrus::Value::Type::I64, reinterpret_cast<uint8_t *>(&value)))) {
            return;
        }
        pushByteCode(Walrus::Const64(computeExprResultPosition(Walrus::Value::Type::I64), value), WASMOpcode::I64ConstOpcode);
    }

    virtual void OnF32ConstExpr(uint32_t value) override
    {
        if (processConstValue(Walrus::Value(Walrus::Value::Type::F32, reinterpret_cast<uint8_t *>(&value)))) {
            return;
        }
        pushByteCode(Walrus::Const32(computeExprResultPosition(Walrus::Value::Type::F32), value), WASMOpcode::F32ConstOpcode);
    }

    virtual void OnF64ConstExpr(uint64_t value) override
    {
        if (processConstValue(Walrus::Value(Walrus::Value::Type::F64, reinterpret_cast<uint8_t *>(&value)))) {
            return;
        }
        pushByteCode(Walrus::Const64(computeExprResultPosition(Walrus::Value::Type::F64), value), WASMOpcode::F64ConstOpcode);
    }

    virtual void OnV128ConstExpr(uint8_t *value) override
    {
        if (processConstValue(Walrus::Value(Walrus::Value::Type::V128, value))) {
            return;
        }
        pushByteCode(Walrus::Const128(computeExprResultPosition(Walrus::Value::Type::V128), value), WASMOpcode::V128ConstOpcode);
    }

    size_t computeExprResultPosition(Walrus::Value::Type type)
    {
        if (!m_preprocessData.m_inPreprocess) {
            // if there is local.set code ahead,
            // we can use local variable position as expr target position
            auto localSetInfo = readAheadLocalGetIfExists();
            if (localSetInfo.first) {
                auto pos = m_localInfo[localSetInfo.first.value()].m_position;
                // skip local.set opcode
                *m_readerOffsetPointer += localSetInfo.second;
                return pos;
            }
        }

        return pushVMStack(type);
    }

    virtual void OnLocalGetExpr(Index localIndex) override
    {
        auto localPos = m_localInfo[localIndex].m_position;
        auto localValueType = m_localInfo[localIndex].m_valueType;

        bool canUseDirectReference = true;
        size_t pos = *m_readerOffsetPointer;
        for (const auto &r : m_preprocessData.m_localVariableInfo[localIndex].m_usageInfo) {
            if (r.m_startPosition <= pos && pos <= r.m_endPosition) {
                if (r.m_hasWriteUsage) {
                    canUseDirectReference = false;
                    break;
                }
            }
        }

        if (canUseDirectReference) {
            pushVMStack(localValueType, localPos, localIndex);
        } else {
            auto pos = m_functionStackSizeSoFar;
            pushVMStack(localValueType, pos, localIndex);
            generateMoveCodeIfNeeds(localPos, pos, localValueType);
        }
    }

    virtual void OnLocalSetExpr(Index localIndex) override
    {
        auto localPos = m_localInfo[localIndex].m_position;

        ASSERT(m_localInfo[localIndex].m_valueType == peekVMStackValueType());
        auto src = popVMStackInfo();
        generateMoveCodeIfNeeds(src.position(), localPos, src.valueType());
        m_preprocessData.addLocalVariableWrite(localIndex);
    }

    virtual void OnLocalTeeExpr(Index localIndex) override
    {
        auto valueType = m_localInfo[localIndex].m_valueType;
        auto localPos = m_localInfo[localIndex].m_position;
        ASSERT(valueType == peekVMStackValueType());
        auto dstInfo = peekVMStackInfo();
        generateMoveCodeIfNeeds(dstInfo.position(), localPos, valueType);
        m_preprocessData.addLocalVariableWrite(localIndex);
    }

    virtual void OnGlobalGetExpr(Index index) override
    {
        auto valueType = m_result.m_globalTypes[index]->type();
        auto sz = Walrus::valueSize(valueType);
        auto stackPos = computeExprResultPosition(valueType);
        if (sz == 4) {
            pushByteCode(Walrus::GlobalGet32(stackPos, index), WASMOpcode::GlobalGetOpcode);
        } else if (sz == 8) {
            pushByteCode(Walrus::GlobalGet64(stackPos, index), WASMOpcode::GlobalGetOpcode);
        } else {
            ASSERT(sz == 16);
            pushByteCode(Walrus::GlobalGet128(stackPos, index), WASMOpcode::GlobalGetOpcode);
        }
    }

    virtual void OnGlobalSetExpr(Index index) override
    {
        auto valueType = m_result.m_globalTypes[index]->type();
        auto stackPos = peekVMStack();

        ASSERT(peekVMStackValueType() == valueType);
        auto sz = Walrus::valueSize(valueType);
        if (sz == 4) {
            pushByteCode(Walrus::GlobalSet32(stackPos, index), WASMOpcode::GlobalSetOpcode);
        } else if (sz == 8) {
            pushByteCode(Walrus::GlobalSet64(stackPos, index), WASMOpcode::GlobalSetOpcode);
        } else {
            pushByteCode(Walrus::GlobalSet128(stackPos, index), WASMOpcode::GlobalSetOpcode);
        }
        popVMStack();
    }

    virtual void OnDropExpr() override
    {
        popVMStack();
    }

    virtual void OnBinaryExpr(uint32_t opcode) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[1]) == peekVMStackValueType());
        auto src1 = popVMStack();
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[0]) == peekVMStackValueType());
        auto src0 = popVMStack();
        auto dst = computeExprResultPosition(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType));
        generateBinaryCode(code, src0, src1, dst);
    }

    virtual void OnUnaryExpr(uint32_t opcode) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[0]) == peekVMStackValueType());
        auto src = popVMStack();
        auto dst = computeExprResultPosition(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType));
        generateUnaryCode(code, src, dst);
    }

    virtual void OnTernaryExpr(uint32_t opcode) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[2]) == peekVMStackValueType());
        auto c = popVMStack();
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[1]) == peekVMStackValueType());
        auto rhs = popVMStack();
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[0]) == peekVMStackValueType());
        auto lhs = popVMStack();
        auto dst = computeExprResultPosition(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType));
        switch (code) {
        case WASMOpcode::V128BitSelectOpcode:
            pushByteCode(Walrus::V128BitSelect(lhs, rhs, c, dst), code);
            break;
        default:
            ASSERT_NOT_REACHED();
            break;
        }
    }

    virtual void OnIfExpr(Type sigType) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto stackPos = popVMStack();

        bool isInverted = canBeInverted(stackPos);
        if (UNLIKELY(isInverted)) {
            stackPos = m_currentFunction->peekByteCode<Walrus::UnaryOperation>(m_lastI32EqzPos)->srcOffset();
            m_currentFunction->resizeByteCode(m_lastI32EqzPos);
            m_lastI32EqzPos = s_noI32Eqz;
        }

        BlockInfo b(BlockInfo::IfElse, sigType, *this);
        b.m_jumpToEndBrInfo.push_back({ BlockInfo::JumpToEndBrInfo::IsJumpIf, b.m_position });
        m_blockInfo.push_back(b);

        if (UNLIKELY(isInverted)) {
            pushByteCode(Walrus::JumpIfTrue(stackPos), WASMOpcode::IfOpcode);
        } else {
            pushByteCode(Walrus::JumpIfFalse(stackPos), WASMOpcode::IfOpcode);
        }
        m_preprocessData.seenBranch();
    }

    void restoreVMStackBy(const BlockInfo &blockInfo)
    {
        if (blockInfo.m_vmStack.size() <= m_vmStack.size()) {
            size_t diff = m_vmStack.size() - blockInfo.m_vmStack.size();
            for (size_t i = 0; i < diff; i++) {
                popVMStack();
            }
            ASSERT(blockInfo.m_vmStack.size() == m_vmStack.size());
        }
        m_vmStack = blockInfo.m_vmStack;
        m_functionStackSizeSoFar = blockInfo.m_functionStackSizeSoFar;
    }

    void keepBlockResultsIfNeeds(BlockInfo &blockInfo)
    {
        auto dropSize = dropStackValuesBeforeBrIfNeeds(0);
        keepBlockResultsIfNeeds(blockInfo, dropSize);
    }

    void keepBlockResultsIfNeeds(BlockInfo &blockInfo, const std::pair<size_t, size_t> &dropSize)
    {
        if (blockInfo.m_shouldRestoreVMStackAtEnd) {
            if (!blockInfo.m_byteCodeGenerationStopped) {
                if (blockInfo.m_returnValueType.IsIndex()) {
                    auto ft = m_result.m_functionTypes[blockInfo.m_returnValueType];
                    const auto &result = ft->result();
                    for (size_t i = 0; i < result.size(); i++) {
                        ASSERT(peekVMStackValueType() == result[result.size() - i - 1]);
                        auto info = peekVMStackInfo();
                        generateMoveCodeIfNeeds(info.position(), info.nonOptimizedPosition(), info.valueType());
                        info.setPosition(info.nonOptimizedPosition());
                        popVMStack();
                    }
                } else if (blockInfo.m_returnValueType != Type::Void) {
                    ASSERT(peekVMStackValueType() == toValueKind(blockInfo.m_returnValueType));
                    auto info = peekVMStackInfo();
                    generateMoveCodeIfNeeds(info.position(), info.nonOptimizedPosition(), info.valueType());
                    info.setPosition(info.nonOptimizedPosition());
                    popVMStack();
                }
            }
        }
    }

    virtual void OnElseExpr() override
    {
        m_preprocessData.seenBranch();
        BlockInfo &blockInfo = m_blockInfo.back();
        keepBlockResultsIfNeeds(blockInfo);

        ASSERT(blockInfo.m_blockType == BlockInfo::IfElse);
        blockInfo.m_jumpToEndBrInfo.erase(blockInfo.m_jumpToEndBrInfo.begin());

        if (!blockInfo.m_byteCodeGenerationStopped) {
            blockInfo.m_jumpToEndBrInfo.push_back({ BlockInfo::JumpToEndBrInfo::IsJump, m_currentFunction->currentByteCodeSize() });
            pushByteCode(Walrus::Jump(), WASMOpcode::ElseOpcode);
        }

        blockInfo.m_byteCodeGenerationStopped = false;
        restoreVMStackBy(blockInfo);
        m_currentFunction->peekByteCode<Walrus::JumpIfFalse>(blockInfo.m_position)
            ->setOffset(m_currentFunction->currentByteCodeSize() - blockInfo.m_position);
    }

    virtual void OnLoopExpr(Type sigType) override
    {
        BlockInfo b(BlockInfo::Loop, sigType, *this);
        m_blockInfo.push_back(b);
    }

    virtual void OnBlockExpr(Type sigType) override
    {
        BlockInfo b(BlockInfo::Block, sigType, *this);
        m_blockInfo.push_back(b);
    }

    BlockInfo &findBlockInfoInBr(Index depth)
    {
        ASSERT(m_blockInfo.size());
        auto iter = m_blockInfo.rbegin();
        while (depth) {
            iter++;
            depth--;
        }
        return *iter;
    }

    void stopToGenerateByteCodeWhileBlockEnd()
    {
        if (m_resumeGenerateByteCodeAfterNBlockEnd) {
            return;
        }

        if (m_blockInfo.size()) {
            m_resumeGenerateByteCodeAfterNBlockEnd = 1;
            auto &blockInfo = m_blockInfo.back();
            blockInfo.m_shouldRestoreVMStackAtEnd = true;
            blockInfo.m_byteCodeGenerationStopped = true;
        } else {
            while (m_vmStack.size()) {
                popVMStack();
            }
        }
        m_shouldContinueToGenerateByteCode = false;
    }

    // return drop size, parameter size
    std::pair<size_t, size_t> dropStackValuesBeforeBrIfNeeds(Index depth)
    {
        size_t dropValueSize = 0;
        size_t parameterSize = 0;
        if (depth < m_blockInfo.size()) {
            auto iter = m_blockInfo.rbegin() + depth;
            if (iter->m_vmStack.size() < m_vmStack.size()) {
                size_t start = iter->m_vmStack.size();
                for (size_t i = start; i < m_vmStack.size(); i++) {
                    dropValueSize += m_vmStack[i].stackAllocatedSize();
                }

                if (iter->m_blockType == BlockInfo::Loop) {
                    if (iter->m_returnValueType.IsIndex()) {
                        auto ft = m_result.m_functionTypes[iter->m_returnValueType];
                        dropValueSize += ft->paramStackSize();
                        parameterSize += ft->paramStackSize();
                    }
                } else {
                    if (iter->m_returnValueType.IsIndex()) {
                        auto ft = m_result.m_functionTypes[iter->m_returnValueType];
                        const auto &result = ft->result();
                        for (size_t i = 0; i < result.size(); i++) {
                            parameterSize += Walrus::valueStackAllocatedSize(result[i]);
                        }
                    } else if (iter->m_returnValueType != Type::Void) {
                        parameterSize += Walrus::valueStackAllocatedSize(toValueKind(iter->m_returnValueType));
                    }
                }
            }
        } else if (m_blockInfo.size()) {
            auto iter = m_blockInfo.begin();
            size_t start = iter->m_vmStack.size();
            for (size_t i = start; i < m_vmStack.size(); i++) {
                dropValueSize += m_vmStack[i].stackAllocatedSize();
            }
        }

        return std::make_pair(dropValueSize, parameterSize);
    }

    void generateMoveCodeIfNeeds(size_t srcPosition, size_t dstPosition, Walrus::Value::Type type)
    {
        if (srcPosition != dstPosition) {
            switch (type) {
            case Walrus::Value::I32:
                pushByteCode(Walrus::MoveI32(srcPosition, dstPosition), WASMOpcode::MoveI32Opcode);
                break;
            case Walrus::Value::F32:
                pushByteCode(Walrus::MoveF32(srcPosition, dstPosition), WASMOpcode::MoveF32Opcode);
                break;
            case Walrus::Value::I64:
                pushByteCode(Walrus::MoveI64(srcPosition, dstPosition), WASMOpcode::MoveI64Opcode);
                break;
            case Walrus::Value::F64:
                pushByteCode(Walrus::MoveF64(srcPosition, dstPosition), WASMOpcode::MoveF64Opcode);
                break;
            case Walrus::Value::V128:
                pushByteCode(Walrus::MoveV128(srcPosition, dstPosition), WASMOpcode::MoveV128Opcode);
                break;
            default:
                ASSERT(type == Walrus::Value::FuncRef || type == Walrus::Value::ExternRef);

                if (sizeof(size_t) == 4) {
                    pushByteCode(Walrus::MoveI32(srcPosition, dstPosition), WASMOpcode::MoveI32Opcode);
                } else {
                    pushByteCode(Walrus::MoveI64(srcPosition, dstPosition), WASMOpcode::MoveI64Opcode);
                }
                break;
            }
        }
    }

    void generateMoveValuesCodeRegardToDrop(std::pair<size_t, size_t> dropSize)
    {
        ASSERT(dropSize.second);
        int64_t remainSize = dropSize.second;
        auto srcIter = m_vmStack.rbegin();
        while (true) {
            remainSize -= srcIter->stackAllocatedSize();
            if (!remainSize) {
                break;
            }
            if (remainSize < 0) {
                // stack mismatch! we don't need to generate code
                return;
            }
            srcIter++;
        }

        remainSize = dropSize.first;
        auto dstIter = m_vmStack.rbegin();
        while (true) {
            remainSize -= dstIter->stackAllocatedSize();
            if (!remainSize) {
                break;
            }
            if (remainSize < 0) {
                // stack mismatch! we don't need to generate code
                return;
            }
            dstIter++;
        }

        // reverse order copy to protect newer values
        remainSize = dropSize.second;
        while (true) {
            generateMoveCodeIfNeeds(srcIter->position(), dstIter->nonOptimizedPosition(), srcIter->valueType());
            remainSize -= srcIter->stackAllocatedSize();
            if (!remainSize) {
                break;
            }
            srcIter--;
            dstIter--;
        }
    }

    void generateEndCode(bool shouldClearVMStack = false)
    {
        if (UNLIKELY(m_currentFunctionType->result().size() > m_vmStack.size())) {
            // error case of global init expr
            return;
        }
        auto pos = m_currentFunction->currentByteCodeSize();
        auto offsetCount = computeFunctionParameterOrResultOffsetCount(m_currentFunctionType->result());
        pushByteCode(Walrus::End(offsetCount), WASMOpcode::EndOpcode);

        auto &result = m_currentFunctionType->result();
        m_currentFunction->expandByteCode(Walrus::ByteCode::pointerAlignedSize(sizeof(Walrus::ByteCodeStackOffset) * offsetCount));
        ASSERT(m_currentFunction->currentByteCodeSize() % sizeof(void *) == 0);
        Walrus::End *end = m_currentFunction->peekByteCode<Walrus::End>(pos);
        size_t offsetIndex = 0;
        for (size_t i = 0; i < result.size(); i++) {
            auto type = result[result.size() - 1 - i];
            size_t s = Walrus::valueSize(type);
            size_t offsetSubCount = 0;
            size_t subIndexCount = valueFunctionCopyCount(type);
            for (size_t j = 0; j < s; j += sizeof(size_t)) {
                end->resultOffsets()[offsetCount - offsetIndex - subIndexCount + offsetSubCount++] = (m_vmStack.rbegin() + i)->position() + j;
            }
            offsetIndex += subIndexCount;
        }
        ASSERT(offsetIndex == computeFunctionParameterOrResultOffsetCount(result));

        if (shouldClearVMStack) {
            for (size_t i = 0; i < result.size(); i++) {
                popVMStack();
            }
        }
    }

    void generateFunctionReturnCode(bool shouldClearVMStack = false)
    {
        for (size_t i = 0; i < m_currentFunctionType->result().size(); i++) {
            ASSERT((m_vmStack.rbegin() + i)->valueType() == m_currentFunctionType->result()[m_currentFunctionType->result().size() - i - 1]);
        }
        generateEndCode();
        if (shouldClearVMStack) {
            auto dropSize = dropStackValuesBeforeBrIfNeeds(m_blockInfo.size()).first;
            while (dropSize) {
                dropSize -= popVMStackInfo().stackAllocatedSize();
            }
        } else {
            for (size_t i = 0; i < m_currentFunctionType->result().size(); i++) {
                popVMStack();
            }
            stopToGenerateByteCodeWhileBlockEnd();
        }

        if (!m_blockInfo.size()) {
            // stop to generate bytecode from here!
            m_shouldContinueToGenerateByteCode = false;
            m_resumeGenerateByteCodeAfterNBlockEnd = 0;
        }
    }

    virtual void OnBrExpr(Index depth) override
    {
        m_preprocessData.seenBranch();
        if (m_blockInfo.size() == depth) {
            // this case acts like return
            generateFunctionReturnCode(true);
            return;
        }
        auto &blockInfo = findBlockInfoInBr(depth);
        auto offset = (int32_t)blockInfo.m_position - (int32_t)m_currentFunction->currentByteCodeSize();
        auto dropSize = dropStackValuesBeforeBrIfNeeds(depth);
        if (dropSize.second) {
            generateMoveValuesCodeRegardToDrop(dropSize);
        } else if (blockInfo.m_blockType == BlockInfo::Loop && blockInfo.m_returnValueType.IsIndex() && m_result.m_functionTypes[blockInfo.m_returnValueType]->param().size()) {
            size_t pos = m_currentFunction->currentByteCodeSize();

            auto ft = m_result.m_functionTypes[blockInfo.m_returnValueType];
            const auto &param = ft->param();
            for (size_t i = 0; i < param.size(); i++) {
                ASSERT((m_vmStack.rbegin() + i)->valueType() == param[param.size() - i - 1]);
                auto info = m_vmStack.rbegin() + i;
                generateMoveCodeIfNeeds(info->position(), info->nonOptimizedPosition(), info->valueType());
                info->setPosition(info->nonOptimizedPosition());
            }
        }
        if (blockInfo.m_blockType != BlockInfo::Loop) {
            ASSERT(blockInfo.m_blockType == BlockInfo::Block || blockInfo.m_blockType == BlockInfo::IfElse || blockInfo.m_blockType == BlockInfo::TryCatch);
            blockInfo.m_jumpToEndBrInfo.push_back({ BlockInfo::JumpToEndBrInfo::IsJump, m_currentFunction->currentByteCodeSize() });
        }
        pushByteCode(Walrus::Jump(offset), WASMOpcode::BrOpcode);

        stopToGenerateByteCodeWhileBlockEnd();
    }

    virtual void OnBrIfExpr(Index depth) override
    {
        m_preprocessData.seenBranch();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto stackPos = popVMStack();
        bool isInverted = canBeInverted(stackPos);
        if (UNLIKELY(isInverted)) {
            stackPos = m_currentFunction->peekByteCode<Walrus::UnaryOperation>(m_lastI32EqzPos)->srcOffset();
            m_currentFunction->resizeByteCode(m_lastI32EqzPos);
            m_lastI32EqzPos = s_noI32Eqz;
        }
        if (m_blockInfo.size() == depth) {
            // this case acts like return
            size_t pos = m_currentFunction->currentByteCodeSize();
            if (UNLIKELY(isInverted)) {
                pushByteCode(Walrus::JumpIfTrue(stackPos, sizeof(Walrus::JumpIfTrue) + sizeof(Walrus::End) + sizeof(Walrus::ByteCodeStackOffset) * m_currentFunctionType->result().size()), WASMOpcode::BrIfOpcode);
            } else {
                pushByteCode(Walrus::JumpIfFalse(stackPos, sizeof(Walrus::JumpIfFalse) + sizeof(Walrus::End) + sizeof(Walrus::ByteCodeStackOffset) * m_currentFunctionType->result().size()), WASMOpcode::BrIfOpcode);
            }
            for (size_t i = 0; i < m_currentFunctionType->result().size(); i++) {
                ASSERT((m_vmStack.rbegin() + i)->valueType() == m_currentFunctionType->result()[m_currentFunctionType->result().size() - i - 1]);
            }
            generateEndCode();
            if (UNLIKELY(isInverted)) {
                m_currentFunction->peekByteCode<Walrus::JumpIfTrue>(pos)->setOffset(m_currentFunction->currentByteCodeSize() - pos);
            } else {
                m_currentFunction->peekByteCode<Walrus::JumpIfFalse>(pos)->setOffset(m_currentFunction->currentByteCodeSize() - pos);
            }
            return;
        }

        auto &blockInfo = findBlockInfoInBr(depth);
        auto dropSize = dropStackValuesBeforeBrIfNeeds(depth);
        if (dropSize.second) {
            size_t pos = m_currentFunction->currentByteCodeSize();
            if (UNLIKELY(isInverted)) {
                pushByteCode(Walrus::JumpIfTrue(stackPos), WASMOpcode::BrIfOpcode);
            } else {
                pushByteCode(Walrus::JumpIfFalse(stackPos), WASMOpcode::BrIfOpcode);
            }
            generateMoveValuesCodeRegardToDrop(dropSize);

            auto offset = (int32_t)blockInfo.m_position - (int32_t)m_currentFunction->currentByteCodeSize();
            if (blockInfo.m_blockType != BlockInfo::Loop) {
                ASSERT(blockInfo.m_blockType == BlockInfo::Block || blockInfo.m_blockType == BlockInfo::IfElse || blockInfo.m_blockType == BlockInfo::TryCatch);
                blockInfo.m_jumpToEndBrInfo.push_back({ BlockInfo::JumpToEndBrInfo::IsJump, m_currentFunction->currentByteCodeSize() });
            }
            pushByteCode(Walrus::Jump(offset), WASMOpcode::BrIfOpcode);
            if (UNLIKELY(isInverted)) {
                m_currentFunction->peekByteCode<Walrus::JumpIfTrue>(pos)->setOffset(m_currentFunction->currentByteCodeSize() - pos);
            } else {
                m_currentFunction->peekByteCode<Walrus::JumpIfFalse>(pos)->setOffset(m_currentFunction->currentByteCodeSize() - pos);
            }
        } else if (blockInfo.m_blockType == BlockInfo::Loop && blockInfo.m_returnValueType.IsIndex() && m_result.m_functionTypes[blockInfo.m_returnValueType]->param().size()) {
            size_t pos = m_currentFunction->currentByteCodeSize();
            if (UNLIKELY(isInverted)) {
                pushByteCode(Walrus::JumpIfTrue(stackPos), WASMOpcode::BrIfOpcode);
            } else {
                pushByteCode(Walrus::JumpIfFalse(stackPos), WASMOpcode::BrIfOpcode);
            }

            auto ft = m_result.m_functionTypes[blockInfo.m_returnValueType];
            const auto &param = ft->param();
            for (size_t i = 0; i < param.size(); i++) {
                ASSERT((m_vmStack.rbegin() + i)->valueType() == param[param.size() - i - 1]);
                auto info = m_vmStack.rbegin() + i;
                generateMoveCodeIfNeeds(info->position(), info->nonOptimizedPosition(), info->valueType());
                info->setPosition(info->nonOptimizedPosition());
            }

            auto offset = (int32_t)blockInfo.m_position - (int32_t)m_currentFunction->currentByteCodeSize();
            if (blockInfo.m_blockType != BlockInfo::Loop) {
                ASSERT(blockInfo.m_blockType == BlockInfo::Block || blockInfo.m_blockType == BlockInfo::IfElse || blockInfo.m_blockType == BlockInfo::TryCatch);
                blockInfo.m_jumpToEndBrInfo.push_back({ BlockInfo::JumpToEndBrInfo::IsJump, m_currentFunction->currentByteCodeSize() });
            }
            pushByteCode(Walrus::Jump(offset), WASMOpcode::BrIfOpcode);
            if (UNLIKELY(isInverted)) {
                m_currentFunction->peekByteCode<Walrus::JumpIfTrue>(pos)->setOffset(m_currentFunction->currentByteCodeSize() - pos);
            } else {
                m_currentFunction->peekByteCode<Walrus::JumpIfFalse>(pos)->setOffset(m_currentFunction->currentByteCodeSize() - pos);
            }
        } else {
            auto offset = (int32_t)blockInfo.m_position - (int32_t)m_currentFunction->currentByteCodeSize();
            if (blockInfo.m_blockType != BlockInfo::Loop) {
                ASSERT(blockInfo.m_blockType == BlockInfo::Block || blockInfo.m_blockType == BlockInfo::IfElse || blockInfo.m_blockType == BlockInfo::TryCatch);
                blockInfo.m_jumpToEndBrInfo.push_back({ BlockInfo::JumpToEndBrInfo::IsJumpIf, m_currentFunction->currentByteCodeSize() });
            }
            if (UNLIKELY(isInverted)) {
                pushByteCode(Walrus::JumpIfFalse(stackPos, offset), WASMOpcode::BrIfOpcode);
            } else {
                pushByteCode(Walrus::JumpIfTrue(stackPos, offset), WASMOpcode::BrIfOpcode);
            }
        }
    }

    void emitBrTableCase(size_t brTableCode, Index depth, size_t jumpOffset)
    {
        int32_t offset = (int32_t)(m_currentFunction->currentByteCodeSize() - brTableCode);

        if (m_blockInfo.size() == depth) {
            // this case acts like return
#if !defined(NDEBUG)
            for (size_t i = 0; i < m_currentFunctionType->result().size(); i++) {
                ASSERT((m_vmStack.rbegin() + i)->valueType() == m_currentFunctionType->result()[m_currentFunctionType->result().size() - i - 1]);
            }
#endif
            *(int32_t *)(m_currentFunction->peekByteCode<uint8_t>(brTableCode) + jumpOffset) = offset;
            generateEndCode();
            return;
        }

        auto dropSize = dropStackValuesBeforeBrIfNeeds(depth);

        if (UNLIKELY(dropSize.second)) {
            *(int32_t *)(m_currentFunction->peekByteCode<uint8_t>(brTableCode) + jumpOffset) = offset;
            OnBrExpr(depth);
            return;
        }

        auto &blockInfo = findBlockInfoInBr(depth);

        offset = (int32_t)(blockInfo.m_position - brTableCode);

        if (blockInfo.m_blockType != BlockInfo::Loop) {
            ASSERT(blockInfo.m_blockType == BlockInfo::Block || blockInfo.m_blockType == BlockInfo::IfElse || blockInfo.m_blockType == BlockInfo::TryCatch);
            offset = jumpOffset;
            blockInfo.m_jumpToEndBrInfo.push_back({ BlockInfo::JumpToEndBrInfo::IsBrTable, brTableCode + jumpOffset });
        }

        *(int32_t *)(m_currentFunction->peekByteCode<uint8_t>(brTableCode) + jumpOffset) = offset;
    }

    virtual void OnBrTableExpr(Index numTargets, Index *targetDepths, Index defaultTargetDepth) override
    {
        m_preprocessData.seenBranch();
        ASSERT(peekVMStackValueType() == Walrus::Value::I32);
        auto stackPos = popVMStack();

        size_t brTableCode = m_currentFunction->currentByteCodeSize();
        pushByteCode(Walrus::BrTable(stackPos, numTargets), WASMOpcode::BrTableOpcode);

        if (numTargets) {
            m_currentFunction->expandByteCode(Walrus::ByteCode::pointerAlignedSize(sizeof(int32_t) * numTargets));
            ASSERT(m_currentFunction->currentByteCodeSize() % sizeof(void *) == 0);

            for (Index i = 0; i < numTargets; i++) {
                emitBrTableCase(brTableCode, targetDepths[i], sizeof(Walrus::BrTable) + i * sizeof(int32_t));
            }
        }

        // generate default
        emitBrTableCase(brTableCode, defaultTargetDepth, Walrus::BrTable::offsetOfDefault());
        stopToGenerateByteCodeWhileBlockEnd();
    }

    virtual void OnSelectExpr(Index resultCount, Type *resultTypes) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        ASSERT(resultCount == 0 || resultCount == 1);
        auto stackPos = popVMStack();

        auto type = peekVMStackValueType();
        auto src1 = popVMStack();
        auto src0 = popVMStack();
        auto dst = computeExprResultPosition(type);
        bool isFloat = type == Walrus::Value::F32 || type == Walrus::Value::F64;
        pushByteCode(Walrus::Select(stackPos, Walrus::valueSize(type), isFloat, src0, src1, dst), WASMOpcode::SelectOpcode);
    }

    virtual void OnThrowExpr(Index tagIndex) override
    {
        m_preprocessData.seenBranch();
        auto pos = m_currentFunction->currentByteCodeSize();
        uint32_t offsetsSize = 0;

        if (tagIndex != std::numeric_limits<Index>::max()) {
            offsetsSize = m_result.m_functionTypes[m_result.m_tagTypes[tagIndex]->sigIndex()]->param().size();
        }

        pushByteCode(Walrus::Throw(tagIndex, offsetsSize), WASMOpcode::ThrowOpcode);

        if (tagIndex != std::numeric_limits<Index>::max()) {
            auto functionType = m_result.m_functionTypes[m_result.m_tagTypes[tagIndex]->sigIndex()];
            auto &param = functionType->param();
            m_currentFunction->expandByteCode(Walrus::ByteCode::pointerAlignedSize(sizeof(Walrus::ByteCodeStackOffset) * param.size()));
            ASSERT(m_currentFunction->currentByteCodeSize() % sizeof(void *) == 0);
            Walrus::Throw *code = m_currentFunction->peekByteCode<Walrus::Throw>(pos);
            for (size_t i = 0; i < param.size(); i++) {
                code->dataOffsets()[param.size() - i - 1] = (m_vmStack.rbegin() + i)->position();
            }
            for (size_t i = 0; i < param.size(); i++) {
                ASSERT(peekVMStackValueType() == functionType->param()[functionType->param().size() - i - 1]);
                popVMStack();
            }
        }

        stopToGenerateByteCodeWhileBlockEnd();
    }

    virtual void OnTryExpr(Type sigType) override
    {
        BlockInfo b(BlockInfo::TryCatch, sigType, *this);
        m_blockInfo.push_back(b);
        m_currentFunction->m_hasTryCatch = true;
    }

    void processCatchExpr(Index tagIndex)
    {
        ASSERT(m_blockInfo.back().m_blockType == BlockInfo::TryCatch);

        m_preprocessData.seenBranch();
        auto &blockInfo = m_blockInfo.back();
        keepBlockResultsIfNeeds(blockInfo);
        restoreVMStackBy(blockInfo);

        size_t tryEnd = m_currentFunction->currentByteCodeSize();
        if (m_catchInfo.size() && m_catchInfo.back().m_tryCatchBlockDepth == m_blockInfo.size()) {
            // not first catch
            tryEnd = m_catchInfo.back().m_tryEnd;
        }

        if (!blockInfo.m_byteCodeGenerationStopped) {
            blockInfo.m_jumpToEndBrInfo.push_back({ BlockInfo::JumpToEndBrInfo::IsJump, m_currentFunction->currentByteCodeSize() });
            pushByteCode(Walrus::Jump(), WASMOpcode::CatchOpcode);
        }

        blockInfo.m_byteCodeGenerationStopped = false;

        m_catchInfo.push_back({ m_blockInfo.size(), m_blockInfo.back().m_position, tryEnd, m_currentFunction->currentByteCodeSize(), tagIndex });

        if (tagIndex != std::numeric_limits<Index>::max()) {
            auto functionType = m_result.m_functionTypes[m_result.m_tagTypes[tagIndex]->sigIndex()];
            for (size_t i = 0; i < functionType->param().size(); i++) {
                pushVMStack(functionType->param()[i]);
            }
        }
    }

    virtual void OnCatchExpr(Index tagIndex) override
    {
        processCatchExpr(tagIndex);
    }

    virtual void OnCatchAllExpr() override
    {
        processCatchExpr(std::numeric_limits<Index>::max());
    }

    virtual void OnMemoryInitExpr(Index segmentIndex, Index memidx) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src2 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src1 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src0 = popVMStack();

        pushByteCode(Walrus::MemoryInit(memidx, segmentIndex, src0, src1, src2), WASMOpcode::MemoryInitOpcode);
    }

    virtual void OnMemoryCopyExpr(Index srcMemIndex, Index dstMemIndex) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src2 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src1 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src0 = popVMStack();

        pushByteCode(Walrus::MemoryCopy(srcMemIndex, dstMemIndex, src0, src1, src2), WASMOpcode::MemoryCopyOpcode);
    }

    virtual void OnMemoryFillExpr(Index memidx) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src2 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src1 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src0 = popVMStack();

        pushByteCode(Walrus::MemoryFill(memidx, src0, src1, src2), WASMOpcode::MemoryFillOpcode);
    }

    virtual void OnDataDropExpr(Index segmentIndex) override
    {
        pushByteCode(Walrus::DataDrop(segmentIndex), WASMOpcode::DataDropOpcode);
    }

    virtual void OnMemoryGrowExpr(Index memidx) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src = popVMStack();
        auto dst = computeExprResultPosition(Walrus::Value::Type::I32);
        pushByteCode(Walrus::MemoryGrow(memidx, src, dst), WASMOpcode::MemoryGrowOpcode);
    }

    virtual void OnMemorySizeExpr(Index memidx) override
    {
        auto stackPos = computeExprResultPosition(Walrus::Value::Type::I32);
        pushByteCode(Walrus::MemorySize(memidx, stackPos), WASMOpcode::MemorySizeOpcode);
    }

    virtual void OnTableGetExpr(Index tableIndex) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src = popVMStack();
        auto dst = computeExprResultPosition(m_result.m_tableTypes[tableIndex]->type());
        pushByteCode(Walrus::TableGet(tableIndex, src, dst), WASMOpcode::TableGetOpcode);
    }

    virtual void OnTableSetExpr(Index tableIndex) override
    {
        ASSERT(peekVMStackValueType() == m_result.m_tableTypes[tableIndex]->type());
        auto src1 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src0 = popVMStack();
        pushByteCode(Walrus::TableSet(tableIndex, src0, src1), WASMOpcode::TableSetOpcode);
    }

    virtual void OnTableGrowExpr(Index tableIndex) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src1 = popVMStack();
        ASSERT(peekVMStackValueType() == m_result.m_tableTypes[tableIndex]->type());
        auto src0 = popVMStack();
        auto dst = computeExprResultPosition(Walrus::Value::Type::I32);
        pushByteCode(Walrus::TableGrow(tableIndex, src0, src1, dst), WASMOpcode::TableGrowOpcode);
    }

    virtual void OnTableSizeExpr(Index tableIndex) override
    {
        auto dst = computeExprResultPosition(Walrus::Value::Type::I32);
        pushByteCode(Walrus::TableSize(tableIndex, dst), WASMOpcode::TableSizeOpcode);
    }

    virtual void OnTableCopyExpr(Index dst_index, Index src_index) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src2 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src1 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src0 = popVMStack();
        pushByteCode(Walrus::TableCopy(dst_index, src_index, src0, src1, src2), WASMOpcode::TableCopyOpcode);
    }

    virtual void OnTableFillExpr(Index tableIndex) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src2 = popVMStack();
        ASSERT(peekVMStackValueType() == m_result.m_tableTypes[tableIndex]->type());
        auto src1 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src0 = popVMStack();
        pushByteCode(Walrus::TableFill(tableIndex, src0, src1, src2), WASMOpcode::TableFillOpcode);
    }

    virtual void OnElemDropExpr(Index segmentIndex) override
    {
        pushByteCode(Walrus::ElemDrop(segmentIndex), WASMOpcode::ElemDropOpcode);
    }

    virtual void OnTableInitExpr(Index segmentIndex, Index tableIndex) override
    {
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src2 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src1 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src0 = popVMStack();
        pushByteCode(Walrus::TableInit(tableIndex, segmentIndex, src0, src1, src2), WASMOpcode::TableInitOpcode);
    }

    virtual void OnLoadExpr(int opcode, Index memidx, Address alignmentLog2, Address offset) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[0]) == peekVMStackValueType());
        auto src = popVMStack();
        auto dst = computeExprResultPosition(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType));
        if ((opcode == (int)WASMOpcode::I32LoadOpcode || opcode == (int)WASMOpcode::F32LoadOpcode) && offset == 0) {
            pushByteCode(Walrus::Load32(src, dst), code);
        } else if ((opcode == (int)WASMOpcode::I64LoadOpcode || opcode == (int)WASMOpcode::F64LoadOpcode) && offset == 0) {
            pushByteCode(Walrus::Load64(src, dst), code);
        } else {
            generateMemoryLoadCode(code, offset, src, dst);
        }
    }

    virtual void OnStoreExpr(int opcode, Index memidx, Address alignmentLog2, Address offset) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[1]) == peekVMStackValueType());
        auto src1 = popVMStack();
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[0]) == peekVMStackValueType());
        auto src0 = popVMStack();
        if ((opcode == (int)WASMOpcode::I32StoreOpcode || opcode == (int)WASMOpcode::F32StoreOpcode) && offset == 0) {
            pushByteCode(Walrus::Store32(src0, src1), code);
        } else if ((opcode == (int)WASMOpcode::I64StoreOpcode || opcode == (int)WASMOpcode::F64StoreOpcode) && offset == 0) {
            pushByteCode(Walrus::Store64(src0, src1), code);
        } else {
            generateMemoryStoreCode(code, offset, src0, src1);
        }
    }

    // Extended Features
#if defined(ENABLE_EXTENDED_FEATURES)
    virtual void OnAtomicLoadExpr(int opcode, Index memidx, Address alignmentLog2, Address offset) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[0]) == peekVMStackValueType());
        auto src = popVMStack();
        auto dst = computeExprResultPosition(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType));
        switch (code) {
#define GENERATE_LOAD_CODE_CASE(name, ...)                  \
    case WASMOpcode::name##Opcode: {                        \
        pushByteCode(Walrus::name(offset, src, dst), code); \
        break;                                              \
    }

            FOR_EACH_BYTECODE_ATOMIC_LOAD_OP(GENERATE_LOAD_CODE_CASE)
#undef GENERATE_LOAD_CODE_CASE
        default:
            ASSERT_NOT_REACHED();
        }
    }

    virtual void OnAtomicStoreExpr(int opcode, Index memidx, Address alignmentLog2, Address offset) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[1]) == peekVMStackValueType());
        auto src1 = popVMStack();
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[0]) == peekVMStackValueType());
        auto src0 = popVMStack();
        switch (code) {
#define GENERATE_STORE_CODE_CASE(name, ...)                   \
    case WASMOpcode::name##Opcode: {                          \
        pushByteCode(Walrus::name(offset, src0, src1), code); \
        break;                                                \
    }

            FOR_EACH_BYTECODE_ATOMIC_STORE_OP(GENERATE_STORE_CODE_CASE)
#undef GENERATE_STORE_CODE_CASE
        default:
            ASSERT_NOT_REACHED();
        }
    }

    virtual void OnAtomicRmwExpr(int opcode, Index memidx, Address alignmentLog2, Address offset) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[1]) == peekVMStackValueType());
        auto src1 = popVMStack();
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[0]) == peekVMStackValueType());
        auto src0 = popVMStack();
        auto dst = computeExprResultPosition(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType));
        switch (code) {
#define GENERATE_RMW_CODE_CASE(name, ...)                          \
    case WASMOpcode::name##Opcode: {                               \
        pushByteCode(Walrus::name(offset, src0, src1, dst), code); \
        break;                                                     \
    }

            FOR_EACH_BYTECODE_ATOMIC_RMW_OP(GENERATE_RMW_CODE_CASE)
#undef GENERATE_RMW_CODE_CASE
        default:
            ASSERT_NOT_REACHED();
        }
    }

    virtual void OnAtomicCmpxchgExpr(int opcode, Index memidx, Address alignmentLog2, Address offset) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[2]) == peekVMStackValueType());
        auto src2 = popVMStack();
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[1]) == peekVMStackValueType());
        auto src1 = popVMStack();
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[0]) == peekVMStackValueType());
        auto src0 = popVMStack();
        auto dst = computeExprResultPosition(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType));
        switch (code) {
#define GENERATE_RMW_CMPXCHG_CODE_CASE(name, ...)                        \
    case WASMOpcode::name##Opcode: {                                     \
        pushByteCode(Walrus::name(offset, src0, src1, src2, dst), code); \
        break;                                                           \
    }

            FOR_EACH_BYTECODE_ATOMIC_RMW_CMPXCHG_OP(GENERATE_RMW_CMPXCHG_CODE_CASE)
#undef GENERATE_RMW_CMPXCHG_CODE_CASE
        default:
            ASSERT_NOT_REACHED();
        }
    }

    virtual void OnAtomicWaitExpr(int opcode, Index memidx, Address alignmentLog2, Address offset) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[2]) == peekVMStackValueType());
        auto src2 = popVMStack();
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[1]) == peekVMStackValueType());
        auto src1 = popVMStack();
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[0]) == peekVMStackValueType());
        auto src0 = popVMStack();
        auto dst = computeExprResultPosition(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType));
        switch (code) {
        case WASMOpcode::MemoryAtomicWait32Opcode: {
            pushByteCode(Walrus::MemoryAtomicWait32(offset, src0, src1, src2, dst), code);
            break;
        }
        case WASMOpcode::MemoryAtomicWait64Opcode: {
            pushByteCode(Walrus::MemoryAtomicWait64(offset, src0, src1, src2, dst), code);
            break;
        }
        default:
            ASSERT_NOT_REACHED();
        }
    }

    virtual void OnAtomicFenceExpr(uint32_t consistency_model) override
    {
        // FIXME do nothing
    }

    virtual void OnAtomicNotifyExpr(int opcode, Index memidx, Address alignmentLog2, Address offset) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(code == WASMOpcode::MemoryAtomicNotifyOpcode);
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[1]) == peekVMStackValueType());
        auto src1 = popVMStack();
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[0]) == peekVMStackValueType());
        auto src0 = popVMStack();
        auto dst = computeExprResultPosition(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType));
        pushByteCode(Walrus::MemoryAtomicNotify(offset, src0, src1, dst), code);
    }
#else // Extended Features
    virtual void OnAtomicLoadExpr(int opcode, Index memidx, Address alignmentLog2, Address offset) override
    {
        ASSERT_NOT_REACHED();
    }

    virtual void OnAtomicStoreExpr(int opcode, Index memidx, Address alignmentLog2, Address offset) override
    {
        ASSERT_NOT_REACHED();
    }

    virtual void OnAtomicRmwExpr(int opcode, Index memidx, Address alignmentLog2, Address offset) override
    {
        ASSERT_NOT_REACHED();
    }

    virtual void OnAtomicCmpxchgExpr(int opcode, Index memidx, Address alignmentLog2, Address offset) override
    {
        ASSERT_NOT_REACHED();
    }
    virtual void OnAtomicWaitExpr(int opcode, Index memidx, Address alignmentLog2, Address offset) override
    {
        ASSERT_NOT_REACHED();
    }
    virtual void OnAtomicFenceExpr(uint32_t consistency_model) override
    {
        ASSERT_NOT_REACHED();
    }
    virtual void OnAtomicNotifyExpr(int opcode, Index memidx, Address alignmentLog2, Address offset) override
    {
        ASSERT_NOT_REACHED();
    }
#endif // Extended Features

    virtual void OnRefFuncExpr(Index func_index) override
    {
        auto dst = computeExprResultPosition(Walrus::Value::Type::FuncRef);
        pushByteCode(Walrus::RefFunc(func_index, dst), WASMOpcode::RefFuncOpcode);
    }

    virtual void OnRefNullExpr(Type type) override
    {
        Walrus::ByteCodeStackOffset dst = computeExprResultPosition(toValueKind(type));
#if defined(WALRUS_32)
        pushByteCode(Walrus::Const32(dst, Walrus::Value::Null), WASMOpcode::Const32Opcode);
#else
        pushByteCode(Walrus::Const64(dst, Walrus::Value::Null), WASMOpcode::Const64Opcode);
#endif
    }

    virtual void OnRefIsNullExpr() override
    {
        auto src = popVMStack();
        auto dst = computeExprResultPosition(Walrus::Value::Type::I32);
#if defined(WALRUS_32)
        pushByteCode(Walrus::I32Eqz(src, dst), WASMOpcode::RefIsNullOpcode);
#else
        pushByteCode(Walrus::I64Eqz(src, dst), WASMOpcode::RefIsNullOpcode);
#endif
    }

    virtual void OnNopExpr() override
    {
    }

    virtual void OnReturnExpr() override
    {
        m_preprocessData.seenBranch();
        generateFunctionReturnCode();
    }

    virtual void OnEndExpr() override
    {
        // combining an i32.eqz at the end of a block followed by a JumpIf cannot be combined
        // because it is possible to jump to the location after i32.eqz
        m_lastI32EqzPos = s_noI32Eqz;
        if (m_blockInfo.size()) {
            auto dropSize = dropStackValuesBeforeBrIfNeeds(0);
            auto blockInfo = m_blockInfo.back();
            m_blockInfo.pop_back();

#if !defined(NDEBUG)
            if (!blockInfo.m_shouldRestoreVMStackAtEnd) {
                if (!blockInfo.m_returnValueType.IsIndex() && blockInfo.m_returnValueType != Type::Void) {
                    ASSERT(peekVMStackValueType() == toValueKind(blockInfo.m_returnValueType));
                }
            }
#endif

            if (blockInfo.m_blockType == BlockInfo::TryCatch) {
                auto iter = m_catchInfo.begin();
                while (iter != m_catchInfo.end()) {
                    if (iter->m_tryCatchBlockDepth - 1 != m_blockInfo.size()) {
                        iter++;
                        continue;
                    }
                    size_t stackSizeToBe = m_initialFunctionStackSize;
                    for (size_t i = 0; i < blockInfo.m_vmStack.size(); i++) {
                        stackSizeToBe += m_vmStack[i].stackAllocatedSize();
                    }
                    m_currentFunction->m_catchInfo.push_back({ iter->m_tryStart, iter->m_tryEnd, iter->m_catchStart, stackSizeToBe, iter->m_tagIndex });
                    iter = m_catchInfo.erase(iter);
                }
            }

            if (blockInfo.m_byteCodeGenerationStopped && blockInfo.m_jumpToEndBrInfo.size() == 0) {
                stopToGenerateByteCodeWhileBlockEnd();
                return;
            }

            keepBlockResultsIfNeeds(blockInfo, dropSize);

            if (blockInfo.m_shouldRestoreVMStackAtEnd) {
                restoreVMStackBy(blockInfo);
                if (blockInfo.m_returnValueType.IsIndex()) {
                    auto ft = m_result.m_functionTypes[blockInfo.m_returnValueType];
                    const auto &param = ft->param();
                    for (size_t i = 0; i < param.size(); i++) {
                        ASSERT(peekVMStackValueType() == param[param.size() - i - 1]);
                        popVMStack();
                    }

                    const auto &result = ft->result();
                    for (size_t i = 0; i < result.size(); i++) {
                        pushVMStack(result[i]);
                    }
                } else if (blockInfo.m_returnValueType != Type::Void) {
                    pushVMStack(toValueKind(blockInfo.m_returnValueType));
                }
            }

            for (size_t i = 0; i < blockInfo.m_jumpToEndBrInfo.size(); i++) {
                switch (blockInfo.m_jumpToEndBrInfo[i].m_type) {
                case BlockInfo::JumpToEndBrInfo::IsJump:
                    m_currentFunction->peekByteCode<Walrus::Jump>(blockInfo.m_jumpToEndBrInfo[i].m_position)->setOffset(m_currentFunction->currentByteCodeSize() - blockInfo.m_jumpToEndBrInfo[i].m_position);
                    break;
                case BlockInfo::JumpToEndBrInfo::IsJumpIf:
                    m_currentFunction->peekByteCode<Walrus::JumpIfFalse>(blockInfo.m_jumpToEndBrInfo[i].m_position)
                        ->setOffset(m_currentFunction->currentByteCodeSize() - blockInfo.m_jumpToEndBrInfo[i].m_position);
                    break;
                default:
                    ASSERT(blockInfo.m_jumpToEndBrInfo[i].m_type == BlockInfo::JumpToEndBrInfo::IsBrTable);

                    int32_t *offset = m_currentFunction->peekByteCode<int32_t>(blockInfo.m_jumpToEndBrInfo[i].m_position);
                    *offset = m_currentFunction->currentByteCodeSize() + (size_t)*offset - blockInfo.m_jumpToEndBrInfo[i].m_position;
                    break;
                }
            }
        } else {
            generateEndCode(true);
        }
    }

    virtual void OnUnreachableExpr() override
    {
        m_preprocessData.seenBranch();
        pushByteCode(Walrus::Unreachable(), WASMOpcode::UnreachableOpcode);
        stopToGenerateByteCodeWhileBlockEnd();
    }

    virtual void EndFunctionBody(Index index) override
    {
        m_lastI32EqzPos = s_noI32Eqz;
        // #if !defined(NDEBUG)
        //         if (getenv("DUMP_BYTECODE") && strlen(getenv("DUMP_BYTECODE"))) {
        //             m_currentFunction->dumpByteCode();
        //         }
        //         if (m_shouldContinueToGenerateByteCode) {
        //             for (size_t i = 0; i < m_currentFunctionType->result().size() && m_vmStack.size(); i++) {
        //                 ASSERT(popVMStackInfo().valueType() == m_currentFunctionType->result()[m_currentFunctionType->result().size() - i - 1]);
        //             }
        //             ASSERT(m_vmStack.empty());
        //         }
        // #endif

        ASSERT(m_currentFunction == m_result.m_functions[index]);
        endFunction();
    }

    // SIMD Instructions
    virtual void OnLoadSplatExpr(int opcode, Index memidx, Address alignment_log2, Address offset) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src = popVMStack();
        auto dst = computeExprResultPosition(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType));
        switch (code) {
#define GENERATE_LOAD_CODE_CASE(name, ...)                  \
    case WASMOpcode::name##Opcode: {                        \
        pushByteCode(Walrus::name(offset, src, dst), code); \
        break;                                              \
    }

            FOR_EACH_BYTECODE_SIMD_LOAD_SPLAT_OP(GENERATE_LOAD_CODE_CASE)
#undef GENERATE_LOAD_CODE_CASE
        default:
            ASSERT_NOT_REACHED();
        }
    }

    virtual void OnLoadZeroExpr(int opcode, Index memidx, Address alignment_log2, Address offset) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src = popVMStack();
        auto dst = computeExprResultPosition(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType));
        switch (code) {
        case WASMOpcode::V128Load32ZeroOpcode: {
            pushByteCode(Walrus::V128Load32Zero(offset, src, dst), code);
            break;
        }
        case WASMOpcode::V128Load64ZeroOpcode: {
            pushByteCode(Walrus::V128Load64Zero(offset, src, dst), code);
            break;
        }
        default:
            ASSERT_NOT_REACHED();
            break;
        }
    }

    virtual void OnSimdLaneOpExpr(int opcode, uint64_t value) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        switch (code) {
#define GENERATE_SIMD_EXTRACT_LANE_CODE_CASE(name, ...)                                                               \
    case WASMOpcode::name##Opcode: {                                                                                  \
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[0]) == peekVMStackValueType());  \
        auto src = popVMStack();                                                                                      \
        auto dst = computeExprResultPosition(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType)); \
        pushByteCode(Walrus::name(static_cast<uint8_t>(value), src, dst), code);                                      \
        break;                                                                                                        \
    }

#define GENERATE_SIMD_REPLACE_LANE_CODE_CASE(name, ...)                                                               \
    case WASMOpcode::name##Opcode: {                                                                                  \
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[1]) == peekVMStackValueType());  \
        auto src1 = popVMStack();                                                                                     \
        ASSERT(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_paramTypes[0]) == peekVMStackValueType());  \
        auto src0 = popVMStack();                                                                                     \
        auto dst = computeExprResultPosition(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType)); \
        pushByteCode(Walrus::name(static_cast<uint8_t>(value), src0, src1, dst), code);                               \
        break;                                                                                                        \
    }

            FOR_EACH_BYTECODE_SIMD_EXTRACT_LANE_OP(GENERATE_SIMD_EXTRACT_LANE_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_REPLACE_LANE_OP(GENERATE_SIMD_REPLACE_LANE_CODE_CASE)
#undef GENERATE_SIMD_EXTRACT_LANE_CODE_CASE
#undef GENERATE_SIMD_REPLACE_LANE_CODE_CASE
        default:
            ASSERT_NOT_REACHED();
            break;
        }
    }

    virtual void OnSimdLoadLaneExpr(int opcode, Index memidx, Address alignment_log2, Address offset, uint64_t value) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::V128);
        auto src1 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src0 = popVMStack();
        auto dst = computeExprResultPosition(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType));
        switch (code) {
#define GENERATE_LOAD_CODE_CASE(name, opType)                                                                       \
    case WASMOpcode::name##Opcode: {                                                                                \
        pushByteCode(Walrus::name(offset, src0, src1, static_cast<Walrus::ByteCodeStackOffset>(value), dst), code); \
        break;                                                                                                      \
    }
            FOR_EACH_BYTECODE_SIMD_LOAD_LANE_OP(GENERATE_LOAD_CODE_CASE)
#undef GENERATE_LOAD_CODE_CASE
        default:
            ASSERT_NOT_REACHED();
        }
    }

    virtual void OnSimdStoreLaneExpr(int opcode, Index memidx, Address alignment_log2, Address offset, uint64_t value) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::V128);
        auto src1 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::I32);
        auto src0 = popVMStack();
        switch (code) {
#define GENERATE_STORE_CODE_CASE(name, opType)                                                                 \
    case WASMOpcode::name##Opcode: {                                                                           \
        pushByteCode(Walrus::name(offset, src0, src1, static_cast<Walrus::ByteCodeStackOffset>(value)), code); \
        break;                                                                                                 \
    }
            FOR_EACH_BYTECODE_SIMD_STORE_LANE_OP(GENERATE_STORE_CODE_CASE)
#undef GENERATE_STORE_CODE_CASE
        default:
            ASSERT_NOT_REACHED();
        }
    }

    virtual void OnSimdShuffleOpExpr(int opcode, uint8_t *value) override
    {
        ASSERT(static_cast<WASMOpcode>(opcode) == WASMOpcode::I8X16ShuffleOpcode);
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::V128);
        auto src1 = popVMStack();
        ASSERT(peekVMStackValueType() == Walrus::Value::Type::V128);
        auto src0 = popVMStack();
        auto dst = computeExprResultPosition(WASMCodeInfo::codeTypeToValueType(g_wasmCodeInfo[opcode].m_resultType));
        pushByteCode(Walrus::I8X16Shuffle(src0, src1, dst, value), WASMOpcode::I8X16ShuffleOpcode);
    }

    void generateBinaryCode(WASMOpcode code, size_t src0, size_t src1, size_t dst)
    {
        switch (code) {
#define GENERATE_BINARY_CODE_CASE(name, ...)               \
    case WASMOpcode::name##Opcode: {                       \
        pushByteCode(Walrus::name(src0, src1, dst), code); \
        break;                                             \
    }
            FOR_EACH_BYTECODE_BINARY_OP(GENERATE_BINARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_BINARY_OP(GENERATE_BINARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_BINARY_SHIFT_OP(GENERATE_BINARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_BINARY_OTHER(GENERATE_BINARY_CODE_CASE)
#undef GENERATE_BINARY_CODE_CASE
        default:
            ASSERT_NOT_REACHED();
            break;
        }
    }

    void generateUnaryCode(WASMOpcode code, size_t src, size_t dst)
    {
        switch (code) {
#define GENERATE_UNARY_CODE_CASE(name, ...)         \
    case WASMOpcode::name##Opcode: {                \
        pushByteCode(Walrus::name(src, dst), code); \
        break;                                      \
    }
            FOR_EACH_BYTECODE_UNARY_OP(GENERATE_UNARY_CODE_CASE)
            FOR_EACH_BYTECODE_UNARY_OP_2(GENERATE_UNARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_UNARY_OP(GENERATE_UNARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_UNARY_CONVERT_OP(GENERATE_UNARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_UNARY_OTHER(GENERATE_UNARY_CODE_CASE)
#undef GENERATE_UNARY_CODE_CASE
        case WASMOpcode::I32ReinterpretF32Opcode:
            pushByteCode(Walrus::I32ReinterpretF32(src, dst), code);
            break;
        case WASMOpcode::I64ReinterpretF64Opcode:
            pushByteCode(Walrus::I64ReinterpretF64(src, dst), code);
            break;
        case WASMOpcode::F32ReinterpretI32Opcode:
            pushByteCode(Walrus::F32ReinterpretI32(src, dst), code);
            break;
        case WASMOpcode::F64ReinterpretI64Opcode:
            pushByteCode(Walrus::F64ReinterpretI64(src, dst), code);
            break;
        default:
            ASSERT_NOT_REACHED();
            break;
        }
    }

    void generateMemoryLoadCode(WASMOpcode code, size_t offset, size_t src, size_t dst)
    {
        switch (code) {
#define GENERATE_LOAD_CODE_CASE(name, readType, writeType)  \
    case WASMOpcode::name##Opcode: {                        \
        pushByteCode(Walrus::name(offset, src, dst), code); \
        break;                                              \
    }
            FOR_EACH_BYTECODE_LOAD_OP(GENERATE_LOAD_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_LOAD_EXTEND_OP(GENERATE_LOAD_CODE_CASE)
#undef GENERATE_LOAD_CODE_CASE
        default:
            ASSERT_NOT_REACHED();
            break;
        }
    }

    void generateMemoryStoreCode(WASMOpcode code, size_t offset, size_t src0, size_t src1)
    {
        switch (code) {
#define GENERATE_STORE_CODE_CASE(name, readType, writeType)   \
    case WASMOpcode::name##Opcode: {                          \
        pushByteCode(Walrus::name(offset, src0, src1), code); \
        break;                                                \
    }
            FOR_EACH_BYTECODE_STORE_OP(GENERATE_STORE_CODE_CASE)
#undef GENERATE_STORE_CODE_CASE
        default:
            ASSERT_NOT_REACHED();
            break;
        }
    }

    bool isBinaryOperation(WASMOpcode opcode)
    {
        switch (opcode) {
#define GENERATE_BINARY_CODE_CASE(name, op, paramType, returnType) \
    case WASMOpcode::name##Opcode:
            FOR_EACH_BYTECODE_BINARY_OP(GENERATE_BINARY_CODE_CASE)
#undef GENERATE_BINARY_CODE_CASE
            return true;
        default:
            return false;
        }
    }

    Walrus::WASMParsingResult &parsingResult() { return m_result; }

    size_t arrayOffsetFromValue(Walrus::Value::Type type)
    {
#if defined(WALRUS_32)
        switch (type) {
        case Walrus::Value::I32:
        case Walrus::Value::F32:
        case Walrus::Value::ExternRef:
        case Walrus::Value::FuncRef: {
            return 1;
        }
        case Walrus::Value::I64:
        case Walrus::Value::F64: {
            return 2;
        }
        case Walrus::Value::V128: {
            return 4;
        }
        default: {
            break;
        }
        }
#else
        switch (type) {
        case Walrus::Value::I32:
        case Walrus::Value::F32:
        case Walrus::Value::FuncRef:
        case Walrus::Value::ExternRef:
        case Walrus::Value::I64:
        case Walrus::Value::F64: {
            return 1;
        }
        case Walrus::Value::V128: {
            return 2;
        }
        default: {
            break;
        }
        }
#endif
        return 0;
    }


    void setByteCodeDestination(Walrus::ByteCode *byteCode, Walrus::ByteCodeStackOffset position)
    {
        switch (byteCode->opcode()) {
#define GENERATE_UNARY_CODE_CASE(name, ...) case Walrus::ByteCode::name##Opcode:
            FOR_EACH_BYTECODE_UNARY_OP(GENERATE_UNARY_CODE_CASE)
            FOR_EACH_BYTECODE_UNARY_OP_2(GENERATE_UNARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_UNARY_OP(GENERATE_UNARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_UNARY_CONVERT_OP(GENERATE_UNARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_UNARY_OTHER(GENERATE_UNARY_CODE_CASE)
#undef GENERATE_UNARY_CODE_CASE
        case Walrus::ByteCode::Load32Opcode:
        case Walrus::ByteCode::Load64Opcode:
        case Walrus::ByteCode::I64ReinterpretF64Opcode:
        case Walrus::ByteCode::F32ReinterpretI32Opcode:
        case Walrus::ByteCode::F64ReinterpretI64Opcode:
        case Walrus::ByteCode::I32ReinterpretF32Opcode:
        case Walrus::ByteCode::MoveI32Opcode:
        case Walrus::ByteCode::MoveF32Opcode:
        case Walrus::ByteCode::MoveI64Opcode:
        case Walrus::ByteCode::MoveF64Opcode:
        case Walrus::ByteCode::MoveV128Opcode: {
            Walrus::ByteCodeOffset2 *move = reinterpret_cast<Walrus::ByteCodeOffset2 *>(byteCode);
            move->setStackOffset2(position);
            break;
        }
        case Walrus::ByteCode::RefFuncOpcode:
        case Walrus::ByteCode::TableSizeOpcode:
        case Walrus::ByteCode::GlobalGet32Opcode:
        case Walrus::ByteCode::GlobalGet64Opcode:
        case Walrus::ByteCode::GlobalGet128Opcode: {
            Walrus::ByteCodeOffsetValue *get = reinterpret_cast<Walrus::ByteCodeOffsetValue *>(byteCode);
            get->setStackOffset(position);
            break;
        }
#define GENERATE_BINARY_CODE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
            FOR_EACH_BYTECODE_BINARY_OP(GENERATE_BINARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_BINARY_OP(GENERATE_BINARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_BINARY_SHIFT_OP(GENERATE_BINARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_BINARY_OTHER(GENERATE_BINARY_CODE_CASE)
#undef GENERATE_BINARY_CODE_CASE
            {
                Walrus::BinaryOperation *binOp = reinterpret_cast<Walrus::BinaryOperation *>(byteCode);
                binOp->setDstOffset(position);
                break;
            }
        case Walrus::ByteCode::V128Load32ZeroOpcode:
        case Walrus::ByteCode::V128Load64ZeroOpcode:
#define GENERATE_MEMORY_LOAD_CODE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
            FOR_EACH_BYTECODE_LOAD_OP(GENERATE_MEMORY_LOAD_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_LOAD_EXTEND_OP(GENERATE_MEMORY_LOAD_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_LOAD_SPLAT_OP(GENERATE_MEMORY_LOAD_CODE_CASE)
#undef GENERATE_MEMORY_LOAD_CODE_CASE
            {
                Walrus::MemoryLoad *load = reinterpret_cast<Walrus::MemoryLoad *>(byteCode);
                load->setDstOffset(position);
                break;
            }
        case Walrus::ByteCode::SelectOpcode: {
            Walrus::Select *select = reinterpret_cast<Walrus::Select *>(byteCode);
            select->setDstOffset(position);
            break;
        }
        case Walrus::ByteCode::Const32Opcode:
        case Walrus::ByteCode::Const64Opcode:
        case Walrus::ByteCode::Const128Opcode: {
            Walrus::Const32 *constant = reinterpret_cast<Walrus::Const32 *>(byteCode);
            constant->setDstOffset(position);
            break;
        }
        case Walrus::ByteCode::MemorySizeOpcode: {
            Walrus::MemorySize *memSize = reinterpret_cast<Walrus::MemorySize *>(byteCode);
            memSize->setDstOffset(position);
            break;
        }
        case Walrus::ByteCode::MemoryGrowOpcode: {
            Walrus::MemoryGrow *memGrow = reinterpret_cast<Walrus::MemoryGrow *>(byteCode);
            memGrow->setDstOffset(position);
            break;
        }
        case Walrus::ByteCode::TableGetOpcode: {
            Walrus::TableGet *tableGet = reinterpret_cast<Walrus::TableGet *>(byteCode);
            tableGet->setDstOffset(position);
            break;
        }
        case Walrus::ByteCode::TableGrowOpcode: {
            Walrus::TableGrow *tableGrow = reinterpret_cast<Walrus::TableGrow *>(byteCode);
            tableGrow->setDstOffset(position);
            break;
        }
        case Walrus::ByteCode::I8X16ShuffleOpcode: {
            Walrus::I8X16Shuffle *simdShuffle = reinterpret_cast<Walrus::I8X16Shuffle *>(byteCode);
            simdShuffle->setDstOffset(position);
            break;
        }
        case Walrus::ByteCode::V128BitSelectOpcode: {
            Walrus::V128BitSelect *select = reinterpret_cast<Walrus::V128BitSelect *>(byteCode);
            select->setDstOffset(position);
            break;
        }
#define GENERATE_SIMD_MEMORY_LOAD_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
            FOR_EACH_BYTECODE_SIMD_LOAD_LANE_OP(GENERATE_SIMD_MEMORY_LOAD_CASE)
#undef GENERATE_SIMD_MEMORY_LOAD_CODE_CASE
            {
                Walrus::SIMDMemoryLoad *load = reinterpret_cast<Walrus::SIMDMemoryLoad *>(byteCode);
                load->setDstOffset(position);
                break;
            }
#define GENERATE_SIMD_REPLACE_LANE_CODE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
            FOR_EACH_BYTECODE_SIMD_REPLACE_LANE_OP(GENERATE_SIMD_REPLACE_LANE_CODE_CASE)
#undef GENERATE_SIMD_REPLACE_LANE_CODE_CASE
            {
                Walrus::SIMDReplaceLane *lane = reinterpret_cast<Walrus::SIMDReplaceLane *>(byteCode);
                lane->setDstOffset(position);
                break;
            }
#define GENERATE_SIMD_EXTRACT_LANE_CODE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
            FOR_EACH_BYTECODE_SIMD_EXTRACT_LANE_OP(GENERATE_SIMD_EXTRACT_LANE_CODE_CASE)
#undef GENERATE_SIMD_EXTRACT_LANE_CODE_CASE
            {
                Walrus::SIMDExtractLane *lane = reinterpret_cast<Walrus::SIMDExtractLane *>(byteCode);
                lane->setDstOffset(position);
                break;
            }
        default: {
            break;
        }
        }
    }

    void setByteCodeSrc0(Walrus::ByteCode *byteCode, Walrus::ByteCodeStackOffset position)
    {
        switch (byteCode->opcode()) {
#define GENERATE_BINARY_CODE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
            FOR_EACH_BYTECODE_BINARY_OP(GENERATE_BINARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_BINARY_OP(GENERATE_BINARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_BINARY_SHIFT_OP(GENERATE_BINARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_BINARY_OTHER(GENERATE_BINARY_CODE_CASE)
#undef GENERATE_BINARY_CODE_CASE
            {
                Walrus::BinaryOperation *binOp = reinterpret_cast<Walrus::BinaryOperation *>(byteCode);
                binOp->setSrcOffset(position, 0);
                break;
            }
#define GENERATE_UNARY_CODE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
            FOR_EACH_BYTECODE_UNARY_OP(GENERATE_UNARY_CODE_CASE)
            FOR_EACH_BYTECODE_UNARY_OP_2(GENERATE_UNARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_UNARY_OP(GENERATE_UNARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_UNARY_CONVERT_OP(GENERATE_UNARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_UNARY_OTHER(GENERATE_UNARY_CODE_CASE)
#undef GENERATE_UNARY_CODE_CASE
        case Walrus::ByteCode::I64ReinterpretF64Opcode:
        case Walrus::ByteCode::F32ReinterpretI32Opcode:
        case Walrus::ByteCode::F64ReinterpretI64Opcode:
        case Walrus::ByteCode::I32ReinterpretF32Opcode:
        case Walrus::ByteCode::MoveI32Opcode:
        case Walrus::ByteCode::MoveF32Opcode:
        case Walrus::ByteCode::MoveI64Opcode:
        case Walrus::ByteCode::MoveF64Opcode:
        case Walrus::ByteCode::MoveV128Opcode:
        case Walrus::ByteCode::Store32Opcode:
        case Walrus::ByteCode::Store64Opcode:
        case Walrus::ByteCode::Load32Opcode:
        case Walrus::ByteCode::Load64Opcode: {
            Walrus::ByteCodeOffset2 *unOp = reinterpret_cast<Walrus::ByteCodeOffset2 *>(byteCode);
            unOp->setStackOffset1(position);
            break;
        }
        case Walrus::ByteCode::V128Load32ZeroOpcode:
        case Walrus::ByteCode::V128Load64ZeroOpcode:
#define GENERATE_MEMORY_LOAD_CODE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
            FOR_EACH_BYTECODE_LOAD_OP(GENERATE_MEMORY_LOAD_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_LOAD_EXTEND_OP(GENERATE_MEMORY_LOAD_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_LOAD_SPLAT_OP(GENERATE_MEMORY_LOAD_CODE_CASE)
#undef GENERATE_MEMORY_LOAD_CODE_CASE
            {
                Walrus::MemoryLoad *load = reinterpret_cast<Walrus::MemoryLoad *>(byteCode);
                load->setSrcOffset(position);
                break;
            }
#define GENERATE_MEMORY_STORE_CODE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
            FOR_EACH_BYTECODE_STORE_OP(GENERATE_MEMORY_STORE_CODE_CASE)
#undef GENERATE_MEMORY_STORE_CODE_CASE
            {
                Walrus::MemoryStore *store = reinterpret_cast<Walrus::MemoryStore *>(byteCode);
                store->setSrc0Offset(position);
                break;
            }
        case Walrus::ByteCode::SelectOpcode: {
            Walrus::Select *select = reinterpret_cast<Walrus::Select *>(byteCode);
            select->setSrc0Offset(position);
            break;
        }
        case Walrus::ByteCode::JumpIfTrueOpcode:
        case Walrus::ByteCode::JumpIfFalseOpcode:
        case Walrus::ByteCode::GlobalSet32Opcode:
        case Walrus::ByteCode::GlobalSet64Opcode:
        case Walrus::ByteCode::GlobalSet128Opcode: {
            Walrus::ByteCodeOffsetValue *globalSet = reinterpret_cast<Walrus::ByteCodeOffsetValue *>(byteCode);
            globalSet->setStackOffset(position);
            break;
        }
        case Walrus::ByteCode::MemoryGrowOpcode: {
            Walrus::MemoryGrow *memGrow = reinterpret_cast<Walrus::MemoryGrow *>(byteCode);
            memGrow->setSrcOffset(position);
            break;
        }
        case Walrus::ByteCode::MemoryInitOpcode: {
            Walrus::MemoryInit *memInit = reinterpret_cast<Walrus::MemoryInit *>(byteCode);
            memInit->setSrcOffset(position, 0);
            break;
        }
        case Walrus::ByteCode::MemoryCopyOpcode: {
            Walrus::MemoryCopy *memCopy = reinterpret_cast<Walrus::MemoryCopy *>(byteCode);
            memCopy->setSrcOffset(position, 0);
            break;
        }
        case Walrus::ByteCode::MemoryFillOpcode: {
            Walrus::MemoryFill *memFill = reinterpret_cast<Walrus::MemoryFill *>(byteCode);
            memFill->setSrcOffset(position, 0);
            break;
        }
        case Walrus::ByteCode::TableGetOpcode: {
            Walrus::TableGet *tableGet = reinterpret_cast<Walrus::TableGet *>(byteCode);
            tableGet->setSrcOffset(position);
            break;
        }
        case Walrus::ByteCode::TableSetOpcode: {
            Walrus::TableSet *tableSet = reinterpret_cast<Walrus::TableSet *>(byteCode);
            tableSet->setSrc0Offset(position);
            break;
        }
        case Walrus::ByteCode::TableGrowOpcode: {
            Walrus::TableGrow *tableGrow = reinterpret_cast<Walrus::TableGrow *>(byteCode);
            tableGrow->setSrc0Offset(position);
            break;
        }
        case Walrus::ByteCode::TableCopyOpcode: {
            Walrus::TableCopy *tableCopy = reinterpret_cast<Walrus::TableCopy *>(byteCode);
            tableCopy->setSrcOffset(position, 0);
            break;
        }
        case Walrus::ByteCode::TableFillOpcode: {
            Walrus::TableFill *tableFill = reinterpret_cast<Walrus::TableFill *>(byteCode);
            tableFill->setSrcOffset(position, 0);
            break;
        }
        case Walrus::ByteCode::TableInitOpcode: {
            Walrus::TableInit *tableInit = reinterpret_cast<Walrus::TableInit *>(byteCode);
            tableInit->setSrcOffset(position, 0);
            break;
        }
        case Walrus::ByteCode::I8X16ShuffleOpcode: {
            Walrus::I8X16Shuffle *simdShuffle = reinterpret_cast<Walrus::I8X16Shuffle *>(byteCode);
            simdShuffle->setSrcOffset(position, 0);
            break;
        }
        case Walrus::ByteCode::V128BitSelectOpcode: {
            Walrus::V128BitSelect *select = reinterpret_cast<Walrus::V128BitSelect *>(byteCode);
            select->setSrcOffset(position, 0);
            break;
        }
#define GENERATE_SIMD_MEMORY_LOAD_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
            FOR_EACH_BYTECODE_SIMD_LOAD_LANE_OP(GENERATE_SIMD_MEMORY_LOAD_CASE)
#undef GENERATE_SIMD_MEMORY_LOAD_CODE_CASE
            {
                Walrus::SIMDMemoryLoad *load = reinterpret_cast<Walrus::SIMDMemoryLoad *>(byteCode);
                load->setSrc0Offset(position);
                break;
            }
#define GENERATE_SIMD_MEMORY_STORE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
            FOR_EACH_BYTECODE_SIMD_STORE_LANE_OP(GENERATE_SIMD_MEMORY_STORE_CASE)
#undef GENERATE_SIMD_MEMORY_LOAD_CASE
            {
                Walrus::SIMDMemoryStore *store = reinterpret_cast<Walrus::SIMDMemoryStore *>(byteCode);
                store->setSrc0Offset(position);
                break;
            }
#define GENERATE_SIMD_REPLACE_LANE_CODE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
            FOR_EACH_BYTECODE_SIMD_REPLACE_LANE_OP(GENERATE_SIMD_REPLACE_LANE_CODE_CASE)
#undef GENERATE_SIMD_REPLACE_LANE_CODE_CASE
            {
                Walrus::SIMDReplaceLane *lane = reinterpret_cast<Walrus::SIMDReplaceLane *>(byteCode);
                lane->setSrcOffset(position, 0);
                break;
            }
#define GENERATE_SIMD_EXTRACT_LANE_CODE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
            FOR_EACH_BYTECODE_SIMD_EXTRACT_LANE_OP(GENERATE_SIMD_EXTRACT_LANE_CODE_CASE)
#undef GENERATE_SIMD_EXTRACT_LANE_CODE_CASE
            {
                Walrus::SIMDExtractLane *lane = reinterpret_cast<Walrus::SIMDExtractLane *>(byteCode);
                lane->setSrcOffset(position);
                break;
            }
        default: {
            break;
        }
        }
    }

    void setByteCodeSrc1(Walrus::ByteCode *byteCode, Walrus::ByteCodeStackOffset position)
    {
        switch (byteCode->opcode()) {
#define GENERATE_BINARY_CODE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
            FOR_EACH_BYTECODE_BINARY_OP(GENERATE_BINARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_BINARY_OP(GENERATE_BINARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_BINARY_SHIFT_OP(GENERATE_BINARY_CODE_CASE)
            FOR_EACH_BYTECODE_SIMD_BINARY_OTHER(GENERATE_BINARY_CODE_CASE)
#undef GENERATE_BINARY_CODE_CASE
            {
                Walrus::BinaryOperation *binOp = reinterpret_cast<Walrus::BinaryOperation *>(byteCode);
                binOp->setSrcOffset(position, 1);
                break;
            }
#define GENERATE_MEMORY_STORE_CODE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
            FOR_EACH_BYTECODE_STORE_OP(GENERATE_MEMORY_STORE_CODE_CASE)
#undef GENERATE_MEMORY_STORE_CODE_CASE
            {
                Walrus::MemoryStore *store = reinterpret_cast<Walrus::MemoryStore *>(byteCode);
                store->setSrc1Offset(position);
                break;
            }
        case Walrus::ByteCode::Store32Opcode:
        case Walrus::ByteCode::Store64Opcode: {
            Walrus::ByteCodeOffset2 *store = reinterpret_cast<Walrus::ByteCodeOffset2 *>(byteCode);
            store->setStackOffset2(position);
            break;
        }
        case Walrus::ByteCode::SelectOpcode: {
            Walrus::Select *select = reinterpret_cast<Walrus::Select *>(byteCode);
            select->setSrc1Offset(position);
            break;
        }
        case Walrus::ByteCode::MemoryInitOpcode: {
            Walrus::MemoryInit *memInit = reinterpret_cast<Walrus::MemoryInit *>(byteCode);
            memInit->setSrcOffset(position, 1);
            break;
        }
        case Walrus::ByteCode::MemoryCopyOpcode: {
            Walrus::MemoryCopy *memCopy = reinterpret_cast<Walrus::MemoryCopy *>(byteCode);
            memCopy->setSrcOffset(position, 1);
            break;
        }
        case Walrus::ByteCode::MemoryFillOpcode: {
            Walrus::MemoryFill *memFill = reinterpret_cast<Walrus::MemoryFill *>(byteCode);
            memFill->setSrcOffset(position, 1);
            break;
        }
        case Walrus::ByteCode::TableSetOpcode: {
            Walrus::TableSet *tableSet = reinterpret_cast<Walrus::TableSet *>(byteCode);
            tableSet->setSrc1Offset(position);
            break;
        }
        case Walrus::ByteCode::TableGrowOpcode: {
            Walrus::TableGrow *tableGrow = reinterpret_cast<Walrus::TableGrow *>(byteCode);
            tableGrow->setSrc1Offset(position);
            break;
        }
        case Walrus::ByteCode::TableCopyOpcode: {
            Walrus::TableCopy *tableCopy = reinterpret_cast<Walrus::TableCopy *>(byteCode);
            tableCopy->setSrcOffset(position, 1);
            break;
        }
        case Walrus::ByteCode::TableFillOpcode: {
            Walrus::TableFill *tableFill = reinterpret_cast<Walrus::TableFill *>(byteCode);
            tableFill->setSrcOffset(position, 1);
            break;
        }
        case Walrus::ByteCode::TableInitOpcode: {
            Walrus::TableInit *tableInit = reinterpret_cast<Walrus::TableInit *>(byteCode);
            tableInit->setSrcOffset(position, 1);
            break;
        }
        case Walrus::ByteCode::I8X16ShuffleOpcode: {
            Walrus::I8X16Shuffle *simdShuffle = reinterpret_cast<Walrus::I8X16Shuffle *>(byteCode);
            simdShuffle->setSrcOffset(position, 1);
            break;
        }
        case Walrus::ByteCode::V128BitSelectOpcode: {
            Walrus::V128BitSelect *select = reinterpret_cast<Walrus::V128BitSelect *>(byteCode);
            select->setSrcOffset(position, 1);
            break;
        }
#define GENERATE_SIMD_MEMORY_LOAD_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
            FOR_EACH_BYTECODE_SIMD_LOAD_LANE_OP(GENERATE_SIMD_MEMORY_LOAD_CASE)
#undef GENERATE_SIMD_MEMORY_LOAD_CASE
            {
                Walrus::SIMDMemoryLoad *load = reinterpret_cast<Walrus::SIMDMemoryLoad *>(byteCode);
                load->setSrc1Offset(position);
                break;
            }
#define GENERATE_SIMD_MEMORY_STORE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
            FOR_EACH_BYTECODE_SIMD_STORE_LANE_OP(GENERATE_SIMD_MEMORY_STORE_CASE)
#undef GENERATE_SIMD_MEMORY_LOAD_CASE
            {
                Walrus::SIMDMemoryStore *store = reinterpret_cast<Walrus::SIMDMemoryStore *>(byteCode);
                store->setSrc1Offset(position);
                break;
            }
#define GENERATE_SIMD_REPLACE_LANE_CODE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
            FOR_EACH_BYTECODE_SIMD_REPLACE_LANE_OP(GENERATE_SIMD_REPLACE_LANE_CODE_CASE)
#undef GENERATE_SIMD_REPLACE_LANE_CODE_CASE
            {
                Walrus::SIMDReplaceLane *lane = reinterpret_cast<Walrus::SIMDReplaceLane *>(byteCode);
                lane->setSrcOffset(position, 1);
                break;
            }
        default: {
            break;
        }
        }
    }

    void setByteCodeExtra(Walrus::ByteCode *byteCode, Walrus::ByteCodeStackOffset position)
    {
        switch (byteCode->opcode()) {
        case Walrus::ByteCode::SelectOpcode: {
            Walrus::Select *select = reinterpret_cast<Walrus::Select *>(byteCode);
            select->setCondOffset(position);
            break;
        }
        case Walrus::ByteCode::MemoryInitOpcode: {
            Walrus::MemoryInit *memInit = reinterpret_cast<Walrus::MemoryInit *>(byteCode);
            memInit->setSrcOffset(position, 2);
            break;
        }
        case Walrus::ByteCode::MemoryCopyOpcode: {
            Walrus::MemoryCopy *memCopy = reinterpret_cast<Walrus::MemoryCopy *>(byteCode);
            memCopy->setSrcOffset(position, 2);
            break;
        }
        case Walrus::ByteCode::MemoryFillOpcode: {
            Walrus::MemoryFill *memFill = reinterpret_cast<Walrus::MemoryFill *>(byteCode);
            memFill->setSrcOffset(position, 2);
            break;
        }
        case Walrus::ByteCode::TableCopyOpcode: {
            Walrus::TableCopy *tableCopy = reinterpret_cast<Walrus::TableCopy *>(byteCode);
            tableCopy->setSrcOffset(position, 2);
            break;
        }
        case Walrus::ByteCode::TableFillOpcode: {
            Walrus::TableFill *tableFill = reinterpret_cast<Walrus::TableFill *>(byteCode);
            tableFill->setSrcOffset(position, 2);
            break;
        }
        case Walrus::ByteCode::TableInitOpcode: {
            Walrus::TableInit *tableInit = reinterpret_cast<Walrus::TableInit *>(byteCode);
            tableInit->setSrcOffset(position, 2);
            break;
        }
        case Walrus::ByteCode::V128BitSelectOpcode: {
            Walrus::V128BitSelect *select = reinterpret_cast<Walrus::V128BitSelect *>(byteCode);
            select->setSrcOffset(position, 2);
            break;
        }
        default: {
            break;
        }
        }
    }

    void preallocateParameters(std::vector<stackElement *> &slots, std::map<size_t, variableRangeInfo> &variableRange)
    {
        for (size_t i = 0; i < m_currentFunctionType->param().size(); i++) {
            auto range = variableRange.find(m_localInfo[i].m_position);

            if (range == variableRange.end()) {
                RELEASE_ASSERT_NOT_REACHED();
            }

            if (slots.empty()) {
                stackElement *elem = new stackElement({ nullptr, nullptr, static_cast<Walrus::ByteCodeStackOffset>(m_localInfo[i].m_position), 0 });
                range->second.assignedPosition = elem;
                slots.push_back(elem);
            } else {
                stackElement *elem = new stackElement({ slots.back(), nullptr, static_cast<Walrus::ByteCodeStackOffset>(m_localInfo[i].m_position), slots.size() });
                slots.back()->next = elem;
                range->second.assignedPosition = elem;
                slots.push_back(elem);
            }
        }
    }

    void deallocateLocal(std::vector<stackElement *> &slots, variableRangeInfo &range, Walrus::ByteCodeStackOffset &offset,
                         stackElement *free32Slots, stackElement *free64Slots, stackElement *free128Slots)
    {
        stackElement *connected = reinterpret_cast<stackElement *>(-1 - 1);
        stackElement *reserved = reinterpret_cast<stackElement *>(-1 - 2);

        ASSERT(range.assignedPosition != nullptr)

        switch (range.type) {
#if defined(WALRUS_32)
        case Walrus::Value::FuncRef:
        case Walrus::Value::ExternRef:
#endif
        case Walrus::Value::I32:
        case Walrus::Value::F32: {
            if (free32Slots != nullptr) {
                // Check if the slot can be combined with the next one to from a 64 bit slot.
                if (range.assignedPosition->idx + 1 < slots.size()
                    && slots[range.assignedPosition->idx + 1]->pos + 4 % 8 == 0) {
                    if (slots[range.assignedPosition->idx + 1]->next != nullptr
                        || slots[range.assignedPosition->idx + 1] == free32Slots) {
                        // Check if freed slot and a 64 bit slot can be combined into a 128 bit slot.
                        if (free64Slots != nullptr) {
                            if (range.assignedPosition->idx + 3 < slots.size()
                                && slots[range.assignedPosition->idx + 3]->next == connected
                                && range.assignedPosition->pos % 16 == 0) {
                                if (slots[range.assignedPosition->idx + 2]->prev != nullptr) {
                                    slots[range.assignedPosition->idx + 2]->prev->next = slots[range.assignedPosition->idx + 1]->next;

                                    if (slots[range.assignedPosition->idx + 2]->next != nullptr) {
                                        slots[range.assignedPosition->idx + 2]->next->prev = slots[range.assignedPosition->idx + 1]->prev;
                                    }
                                }

                                for (size_t i = 1; i < 4; i++) {
                                    slots[range.assignedPosition->idx + i]->next = connected;
                                    slots[range.assignedPosition->idx + i]->prev = connected;
                                }
                                break;
                            } else if (range.assignedPosition->idx - 2 >= 0
                                       && slots[range.assignedPosition->idx - 1]->next == connected
                                       && slots[range.assignedPosition->idx - 2]->pos % 16 == 0) {
                                if (slots[range.assignedPosition->idx - 2]->prev != nullptr) {
                                    slots[range.assignedPosition->idx - 2]->prev->next = slots[range.assignedPosition->idx - 2]->next;

                                    if (slots[range.assignedPosition->idx - 2]->next != nullptr) {
                                        slots[range.assignedPosition->idx - 2]->next->prev = slots[range.assignedPosition->idx - 2]->prev;
                                    }
                                }
                                for (size_t i = 0; i < 3; i++) {
                                    slots[range.assignedPosition->idx - i]->next = connected;
                                    slots[range.assignedPosition->idx - i]->prev = connected;
                                }
                                break;
                            }
                        } else {
                            // Case when the slots cannot be combined into a 128 bit slot.
                            free64Slots = range.assignedPosition;
                            slots[range.assignedPosition->idx + 1]->next = connected;
                            slots[range.assignedPosition->idx + 1]->prev = connected;
                            break;
                        }
                    }
                    // Check if the slot can be combined with the previous one to from a 64 bit slot.
                } else if (range.assignedPosition->idx - 1 >= 0
                           && slots[range.assignedPosition->idx - 1]->pos % 8 == 0) {
                    if (slots[range.assignedPosition->idx - 1]->next != nullptr
                        || slots[range.assignedPosition->idx - 1] == free32Slots) {
                        // Check if freed slot and a 64 bit slot can be combined into a 128 bit slot.
                        if (free64Slots != nullptr) {
                            if (range.assignedPosition->idx + 2 < slots.size()
                                && slots[range.assignedPosition->idx + 2]->next == connected
                                && range.assignedPosition->pos % 16 == 0) {
                                if (slots[range.assignedPosition->idx + 1]->prev != nullptr) {
                                    slots[range.assignedPosition->idx + 1]->prev->next = slots[range.assignedPosition->idx + 1]->next;

                                    if (slots[range.assignedPosition->idx + 1]->next != nullptr) {
                                        slots[range.assignedPosition->idx + 1]->next->prev = slots[range.assignedPosition->idx + 1]->prev;
                                    }
                                }

                                for (size_t i = 0; i < 3; i++) {
                                    slots[range.assignedPosition->idx + i]->next = connected;
                                    slots[range.assignedPosition->idx + i]->prev = connected;
                                }

                                if (free128Slots != nullptr) {
                                    free128Slots->next = slots[range.assignedPosition->idx - 1];
                                    slots[range.assignedPosition->idx - 1]->prev = free128Slots;
                                    free128Slots = slots[range.assignedPosition->idx - 1];
                                    free128Slots->next = nullptr;
                                } else {
                                    free128Slots = range.assignedPosition;
                                    free128Slots->next = nullptr;
                                    free128Slots->prev = nullptr;
                                }
                                break;
                            } else if (range.assignedPosition->idx - 2 >= 0
                                       && slots[range.assignedPosition->idx - 2]->next == connected
                                       && slots[range.assignedPosition->idx - 2]->pos % 16 == 0) {
                                if (slots[range.assignedPosition->idx - 3]->prev != nullptr) {
                                    slots[range.assignedPosition->idx - 3]->prev->next = slots[range.assignedPosition->idx - 3]->next;

                                    if (slots[range.assignedPosition->idx - 3]->next != nullptr) {
                                        slots[range.assignedPosition->idx - 3]->next->prev = slots[range.assignedPosition->idx - 3]->prev;
                                    }
                                }
                                for (size_t i = 0; i < 3; i++) {
                                    slots[range.assignedPosition->idx - i]->next = connected;
                                    slots[range.assignedPosition->idx - i]->prev = connected;
                                }

                                if (free128Slots != nullptr) {
                                    free128Slots->next = slots[range.assignedPosition->idx - 3];
                                    slots[range.assignedPosition->idx - 3]->prev = free128Slots;
                                    free128Slots = slots[range.assignedPosition->idx - 3];
                                    free128Slots->next = nullptr;
                                } else {
                                    free128Slots = slots[range.assignedPosition->idx - 3];
                                    free128Slots->next = nullptr;
                                    free128Slots->prev = nullptr;
                                }

                                break;
                            }
                        } else {
                            // Case when the slots cannot be combined into a 128 bit slot.
                            free64Slots = range.assignedPosition;
                            slots[range.assignedPosition->idx + 1]->next = connected;
                            slots[range.assignedPosition->idx + 1]->prev = connected;
                            break;
                        }
                    }
                }

                free32Slots->next = range.assignedPosition;
                range.assignedPosition->prev = free32Slots;
                free32Slots = free32Slots->next;
                free32Slots->next = nullptr;
                break;
            } else {
                // If there are no other 32 bit slots then the new slot
                // cannot be paired into a 64 bit slot.
                free32Slots = range.assignedPosition;
                free32Slots->prev = nullptr;
                free32Slots->next = nullptr;
            }
            break;
        }
#if defined(WALRUS_64)
        case Walrus::Value::FuncRef:
        case Walrus::Value::ExternRef:
#endif
        case Walrus::Value::I64:
        case Walrus::Value::F64: {
            if (free64Slots != nullptr) {
                // Check if the slot can be combined with the next one to from a 128 bit slot.
                if (range.assignedPosition->idx + 3 < slots.size()
                    && slots[range.assignedPosition->idx + 3]->next == connected
                    && range.assignedPosition->idx % 16 == 0) {
                    for (size_t i = 1; i < 4; i++) {
                        slots[range.assignedPosition->idx + i]->next = connected;
                        slots[range.assignedPosition->idx + i]->prev = connected;
                    }

                    if (free128Slots != nullptr) {
                        free128Slots->next = range.assignedPosition;
                        range.assignedPosition->prev = free128Slots;
                        free128Slots = range.assignedPosition;
                        free128Slots->next = nullptr;
                    } else {
                        free128Slots = range.assignedPosition;
                        free128Slots->next = nullptr;
                        free128Slots->prev = nullptr;
                    }
                }
                // Check if the slot can be combined with the previous one to from a 128 bit slot.
                else if (range.assignedPosition->idx - 2 >= 0
                         && slots[range.assignedPosition->idx - 2]->next == connected
                         && slots[range.assignedPosition->idx - 2]->pos % 16 == 0) {
                    for (size_t i = 0; i < 3; i++) {
                        slots[range.assignedPosition->idx - 2 + i]->next = connected;
                        slots[range.assignedPosition->idx - 2 + i]->prev = connected;
                    }

                    if (free128Slots != nullptr) {
                        free128Slots->next = slots[range.assignedPosition->idx - 2];
                        slots[range.assignedPosition->idx - 2]->prev = free128Slots;
                        free128Slots = slots[range.assignedPosition->idx - 2];
                        free128Slots->next = nullptr;
                    } else {
                        free128Slots = slots[range.assignedPosition->idx - 2];
                        free128Slots->prev = nullptr;
                        free128Slots->next = nullptr;
                    }
                }
            } else {
                free64Slots = range.assignedPosition;
                free64Slots->next = nullptr;
                free64Slots->prev = nullptr;
            }

            break;
        }
        case Walrus::Value::V128: {
            if (free128Slots != nullptr) {
                free128Slots->next = range.assignedPosition;
                range.assignedPosition->prev = free128Slots;
                free128Slots = range.assignedPosition;
                free128Slots->next = nullptr;
            } else {
                free128Slots = range.assignedPosition;
                free128Slots->next = nullptr;
                free128Slots->prev = nullptr;
            }
            break;
        }
        default: {
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }
        }

        range.assignedPosition = nullptr;
    }

    void allocateLocal(std::vector<stackElement *> &slots, variableRangeInfo &range, Walrus::ByteCodeStackOffset &offset,
                       stackElement *free32Slots, stackElement *free64Slots, stackElement *free128Slots)
    {
        stackElement *connected = reinterpret_cast<stackElement *>(-1 - 1);
        stackElement *reserved = reinterpret_cast<stackElement *>(-1 - 2);

        switch (range.type) {
#if defined(WALRUS_32)
        case Walrus::Value::FuncRef:
        case Walrus::Value::ExternRef:
#endif
        case Walrus::Value::I32:
        case Walrus::Value::F32: {
            if (free32Slots != nullptr) {
                range.assignedPosition = free32Slots;
                range.pos = free32Slots->pos;

                ASSERT(free32Slots->next == nullptr)
                if (free32Slots->prev != nullptr) {
                    free32Slots = free32Slots->prev;
                    free32Slots->next = nullptr;
                } else {
                    free32Slots = nullptr;
                }

                range.assignedPosition->next = reserved;
                range.assignedPosition->prev = reserved;
            } else if (free64Slots != nullptr) {
                range.assignedPosition = free64Slots;
                range.pos = free64Slots->pos;

                ASSERT(free64Slots->next == nullptr)
                ASSERT(free64Slots->idx + 1 <= slots.size())
                if (free32Slots == nullptr) {
                    free32Slots = slots[free64Slots->idx + 1];
                    free32Slots->prev = nullptr;
                    free32Slots->next = nullptr;
                } else {
                    free32Slots->next = slots[free64Slots->idx + 1];
                    free32Slots->next->prev = free32Slots;
                    free32Slots = free32Slots->next;
                    free32Slots->next = nullptr;
                }
                range.assignedPosition->next = reserved;
                range.assignedPosition->prev = reserved;
            } else if (free128Slots != nullptr) {
                range.assignedPosition = free128Slots;
                range.pos = free128Slots->pos;

                ASSERT(free128Slots->idx + 3 <= slots.size())
                if (free32Slots == nullptr) {
                    free32Slots = slots[free128Slots->idx + 1];
                    free32Slots->prev = nullptr;
                    free32Slots->next = nullptr;
                } else {
                    free32Slots->next = slots[free128Slots->idx + 1];
                    free32Slots->next->prev = free32Slots;
                    free32Slots = free32Slots->next;
                    free32Slots->next = nullptr;
                }

                ASSERT(free128Slots->idx + 3 <= slots.size())
                if (free64Slots == nullptr) {
                    free64Slots = slots[free128Slots->idx + 2];
                    free64Slots->prev = nullptr;
                    free64Slots->next = slots[free128Slots->idx + 3];
                    free64Slots->next->next = nullptr;
                    free64Slots = free64Slots->next;
                } else {
                    free64Slots->next = slots[free128Slots->idx + 2];
                    free64Slots->next->prev = free64Slots;
                    free64Slots = free64Slots->next;
                    free64Slots->next = slots[free128Slots->idx + 3];
                    free64Slots->next->prev = free64Slots;
                    free64Slots = free64Slots->next;
                    free64Slots->next = nullptr;
                }

                ASSERT(free128Slots->next != nullptr)
                range.assignedPosition->next = reserved;
                range.assignedPosition->prev = reserved;
            } else {
                stackElement *newElement = new stackElement({ reserved, reserved, offset, slots.size() });
                offset += 4;
                slots.push_back(newElement);
                range.assignedPosition = newElement;
                range.pos = newElement->pos;
            }
            break;
        }
#if defined(WALRUS_64)
        case Walrus::Value::FuncRef:
        case Walrus::Value::ExternRef:
#endif
        case Walrus::Value::I64:
        case Walrus::Value::F64: {
            if (free64Slots != nullptr) {
                range.assignedPosition = free64Slots->prev;
                range.pos = free64Slots->prev->pos;

                ASSERT(free64Slots->next == nullptr)
                // free64Slots->next = nullptr;
                free64Slots = free64Slots->prev;
                free64Slots->next->prev = nullptr;

                free64Slots->next = nullptr;
                if (free64Slots->prev != nullptr) {
                    free64Slots = free64Slots->prev;
                    free64Slots->next->prev = nullptr;
                } else {
                    free64Slots->prev = nullptr;
                    free64Slots = nullptr;
                }
            } else if (free128Slots != nullptr) {
                range.assignedPosition = free128Slots;
                range.pos = free128Slots->pos;

                ASSERT(free128Slots->idx + 3 <= slots.size())

                if (free128Slots->prev != nullptr) {
                    free128Slots = free128Slots->prev;
                } else {
                    free128Slots = nullptr;
                }

                range.assignedPosition->next = reserved;
                range.assignedPosition->prev = reserved;

                size_t idx = range.assignedPosition->idx + 1;
                slots[idx]->next = reserved;
                slots[idx]->prev = reserved;

                idx += 1;
                if (free64Slots != nullptr) {
                    free64Slots->next = slots[idx];
                    free64Slots->next->prev = free64Slots;
                    free64Slots = free64Slots->next;
                    free64Slots->next = nullptr;
                } else {
                    free64Slots = slots[idx];
                    free64Slots->next = nullptr;
                    free64Slots->prev = nullptr;
                }

                idx += 1;
                slots[idx]->next = connected;
                slots[idx]->prev = connected;
            } else {
                stackElement *newElement = new stackElement({ reserved, reserved, offset, slots.size() });
                offset += 4;
                slots.push_back(newElement);
                range.assignedPosition = newElement;
                range.pos = newElement->pos;

                newElement = new stackElement({ reserved, reserved, offset, slots.size() });
                offset += 4;
                slots.push_back(newElement);
            }
            break;
        }
        case Walrus::Value::V128: {
            if (free128Slots != nullptr) {
                range.assignedPosition = free128Slots;
                range.pos = free128Slots->pos;

                if (free128Slots->prev != nullptr) {
                    free128Slots = free128Slots->prev;
                    free128Slots->next = nullptr;
                } else {
                    free128Slots = nullptr;
                }

                for (size_t i = 1; i < 4; i++) {
                    slots[free128Slots->idx + i]->next = reserved;
                    slots[free128Slots->idx + i]->prev = reserved;
                }
            } else {
                stackElement *newElement = new stackElement({ reserved, reserved, offset, slots.size() });
                offset += 4;
                slots.push_back(newElement);
                range.assignedPosition = newElement;
                range.pos = newElement->pos;

                for (size_t i = 0; i < 3; i++) {
                    newElement = new stackElement({ reserved, reserved, offset, slots.size() });
                    offset += 4;
                    slots.push_back(newElement);
                }
            }
            break;
        }
        default: {
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }
        }
    }

    size_t pushInitByteCodes(variableRangeInfo &range)
    {
        size_t constSize = 0;
        switch (range.type) {
#if defined(WALRUS_32)
        case Walrus::Value::ExternRef:
        case Walrus::Value::FuncRef:
#endif
        case Walrus::Value::I32: {
        case Walrus::Value::F32:
            Walrus::Const32 const32 = Walrus::Const32(range.pos, 0);
            char *first = (char *)&const32;

            for (size_t i = 0; i < sizeof(Walrus::Const32); i++) {
                m_currentFunction->m_byteCode.insert(i, *first);
                first++;
            }

            constSize += sizeof(Walrus::Const32);
            break;
        }
#if defined(WALRUS_64)
        case Walrus::Value::ExternRef:
        case Walrus::Value::FuncRef:
#endif
        case Walrus::Value::I64:
        case Walrus::Value::F64: {
            Walrus::Const64 const64 = Walrus::Const64(range.pos, 0);
            char *first = (char *)&const64;

            for (size_t i = 0; i < sizeof(Walrus::Const64); i++) {
                m_currentFunction->m_byteCode.insert(i, *first);
                first++;
            }

            constSize += sizeof(Walrus::Const64);
            break;
        }
        case Walrus::Value::V128: {
            uint8_t empty[16] = {
                0
            };

            Walrus::Const128 const128 = Walrus::Const128(range.pos, empty);
            char *first = (char *)&const128;

            for (size_t i = 0; i < sizeof(Walrus::Const128); i++) {
                m_currentFunction->m_byteCode.insert(i, *first);
                first++;
            }

            constSize += sizeof(Walrus::Const128);
            break;
        }
        default: {
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }
        }

        return constSize;
    }

    void optimizeLocals()
    {
        if (m_currentFunction->functionType()->param().size() == m_localInfo.size()) {
            return;
        }

        // TODO : Write complete / proper sentences.

        // TODO : Add Constants to analysis and params to the algorithm.

        std::function<bool(const size_t, const size_t)> comparator_func = [](const size_t l, size_t r) {
            return l > r;
        };
        std::multimap<size_t, size_t, std::function<bool(size_t, size_t)>> labels(comparator_func);
        std::map<size_t, WASMBinaryReader::variableRangeInfo> variableRange;
        std::vector<Walrus::End *> ends;
        std::vector<Walrus::Call *> calls;
        std::vector<Walrus::CallIndirect *> callIndirects;
        std::vector<Walrus::BrTable *> brTables;
        std::vector<Walrus::Throw *> throws;

        size_t paramSize = m_currentFunctionType->param().size();

        for (unsigned i = 0; i < m_localInfo.size(); i++) {
            variableRangeInfo local = variableRangeInfo(m_localInfo[i].m_position, m_localInfo[i].m_valueType);
            variableRange[m_localInfo[i].m_position] = local;

            if (i < paramSize) {
                variableRange[m_localInfo[i].m_position].start = 0;
                variableRange[m_localInfo[i].m_position].sets.push_back(0);
            }
        }

        for (auto &constant : m_preprocessData.m_constantData) {
            variableRangeInfo elem = variableRangeInfo(constant.second, constant.first.type());
            variableRange[constant.second] = elem;
        }

        size_t i = 0;

        // information collection and naive range finding
        while (i < m_currentFunction->currentByteCodeSize()) {
            Walrus::ByteCode *byteCode = reinterpret_cast<Walrus::ByteCode *>(const_cast<uint8_t *>(m_currentFunction->byteCode() + i));

            // size_t jumpOffset = SIZE_MAX;
            size_t offsets[] = { SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX };

            switch (byteCode->opcode()) {
#define GENERATE_BINARY_CODE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
                FOR_EACH_BYTECODE_BINARY_OP(GENERATE_BINARY_CODE_CASE)
                FOR_EACH_BYTECODE_SIMD_BINARY_OP(GENERATE_BINARY_CODE_CASE)
                FOR_EACH_BYTECODE_SIMD_BINARY_SHIFT_OP(GENERATE_BINARY_CODE_CASE)
                FOR_EACH_BYTECODE_SIMD_BINARY_OTHER(GENERATE_BINARY_CODE_CASE)
                {
                    offsets[0] = reinterpret_cast<Walrus::BinaryOperation *>(byteCode)->dstOffset();
                    offsets[1] = reinterpret_cast<Walrus::BinaryOperation *>(byteCode)->srcOffset()[0];
                    offsets[2] = reinterpret_cast<Walrus::BinaryOperation *>(byteCode)->srcOffset()[1];
                    break;
                }
#undef GENERATE_BINARY_CODE_CASE
#define GENERATE_UNARY_CODE_CASE(name, ...) case Walrus::ByteCode::name##Opcode:
                FOR_EACH_BYTECODE_UNARY_OP(GENERATE_UNARY_CODE_CASE)
                FOR_EACH_BYTECODE_UNARY_OP_2(GENERATE_UNARY_CODE_CASE)
                FOR_EACH_BYTECODE_SIMD_UNARY_OP(GENERATE_UNARY_CODE_CASE)
                FOR_EACH_BYTECODE_SIMD_UNARY_CONVERT_OP(GENERATE_UNARY_CODE_CASE)
                FOR_EACH_BYTECODE_SIMD_UNARY_OTHER(GENERATE_UNARY_CODE_CASE)
#undef GENERATE_UNARY_CODE_CASE
                {
                    offsets[0] = reinterpret_cast<Walrus::UnaryOperation *>(byteCode)->dstOffset();
                    offsets[1] = reinterpret_cast<Walrus::UnaryOperation *>(byteCode)->srcOffset();
                    break;
                }
            case Walrus::ByteCode::I64ReinterpretF64Opcode:
            case Walrus::ByteCode::F32ReinterpretI32Opcode:
            case Walrus::ByteCode::F64ReinterpretI64Opcode:
            case Walrus::ByteCode::I32ReinterpretF32Opcode:
            case Walrus::ByteCode::MoveI32Opcode:
            case Walrus::ByteCode::MoveF32Opcode:
            case Walrus::ByteCode::MoveI64Opcode:
            case Walrus::ByteCode::MoveF64Opcode:
            case Walrus::ByteCode::MoveV128Opcode: {
                offsets[0] = reinterpret_cast<Walrus::ByteCodeOffset2 *>(byteCode)->stackOffset2();
                offsets[1] = reinterpret_cast<Walrus::ByteCodeOffset2 *>(byteCode)->stackOffset1();
                break;
            }
            case Walrus::ByteCode::GlobalGet32Opcode:
            case Walrus::ByteCode::GlobalGet64Opcode:
            case Walrus::ByteCode::GlobalGet128Opcode: {
                offsets[0] = reinterpret_cast<Walrus::GlobalGet32 *>(byteCode)->dstOffset();
                break;
            }
            case Walrus::ByteCode::GlobalSet32Opcode:
            case Walrus::ByteCode::GlobalSet64Opcode:
            case Walrus::ByteCode::GlobalSet128Opcode: {
                offsets[1] = reinterpret_cast<Walrus::GlobalSet32 *>(byteCode)->srcOffset();
                break;
            }
            case Walrus::ByteCode::Load32Opcode:
            case Walrus::ByteCode::Load64Opcode: {
                offsets[0] = reinterpret_cast<Walrus::Load32 *>(byteCode)->dstOffset();
                offsets[1] = reinterpret_cast<Walrus::Load32 *>(byteCode)->srcOffset();
                break;
            }
            case Walrus::ByteCode::Store32Opcode:
            case Walrus::ByteCode::Store64Opcode: {
                offsets[1] = reinterpret_cast<Walrus::Store32 *>(byteCode)->src0Offset();
                offsets[2] = reinterpret_cast<Walrus::Store32 *>(byteCode)->src1Offset();
                break;
            }
            case Walrus::ByteCode::V128Load32ZeroOpcode:
            case Walrus::ByteCode::V128Load64ZeroOpcode:
#define GENERATE_MEMORY_LOAD_CODE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
                FOR_EACH_BYTECODE_LOAD_OP(GENERATE_MEMORY_LOAD_CODE_CASE)
                FOR_EACH_BYTECODE_SIMD_LOAD_EXTEND_OP(GENERATE_MEMORY_LOAD_CODE_CASE)
                FOR_EACH_BYTECODE_SIMD_LOAD_SPLAT_OP(GENERATE_MEMORY_LOAD_CODE_CASE)
#undef GENERATE_MEMORY_LOAD_CODE_CASE
                {
                    offsets[0] = reinterpret_cast<Walrus::MemoryLoad *>(byteCode)->dstOffset();
                    offsets[1] = reinterpret_cast<Walrus::MemoryLoad *>(byteCode)->srcOffset();
                    break;
                }
#define GENERATE_SIMD_MEMORY_LOAD_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
                FOR_EACH_BYTECODE_SIMD_LOAD_LANE_OP(GENERATE_SIMD_MEMORY_LOAD_CASE)
#undef GENERATE_SIMD_MEMORY_LOAD_CASE
                {
                    offsets[0] = reinterpret_cast<Walrus::SIMDMemoryLoad *>(byteCode)->dstOffset();
                    offsets[1] = reinterpret_cast<Walrus::SIMDMemoryLoad *>(byteCode)->src0Offset();
                    offsets[2] = reinterpret_cast<Walrus::SIMDMemoryLoad *>(byteCode)->src1Offset();
                    break;
                }
#define GENERATE_MEMORY_STORE_CODE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
                FOR_EACH_BYTECODE_STORE_OP(GENERATE_MEMORY_STORE_CODE_CASE)
#undef GENERATE_MEMORY_STORE_CODE_CASE
                {
                    offsets[1] = reinterpret_cast<Walrus::MemoryStore *>(byteCode)->src0Offset();
                    offsets[2] = reinterpret_cast<Walrus::MemoryStore *>(byteCode)->src1Offset();
                    break;
                }
#define GENERATE_SIMD_MEMORY_STORE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
                FOR_EACH_BYTECODE_SIMD_STORE_LANE_OP(GENERATE_SIMD_MEMORY_STORE_CASE)
#undef GENERATE_SIMD_MEMORY_STORE_CASE
                {
                    offsets[1] = reinterpret_cast<Walrus::SIMDMemoryStore *>(byteCode)->src0Offset();
                    offsets[2] = reinterpret_cast<Walrus::SIMDMemoryStore *>(byteCode)->src1Offset();
                    break;
                }
#define GENERATE_SIMD_EXTRACT_LANE_CODE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
                FOR_EACH_BYTECODE_SIMD_EXTRACT_LANE_OP(GENERATE_SIMD_EXTRACT_LANE_CODE_CASE)
#undef GENERATE_SIMD_EXTRACT_LANE_CODE_CASE
                {
                    offsets[0] = reinterpret_cast<Walrus::SIMDExtractLane *>(byteCode)->dstOffset();
                    offsets[1] = reinterpret_cast<Walrus::SIMDExtractLane *>(byteCode)->srcOffset();
                    break;
                }
#define GENERATE_SIMD_REPLACE_LANE_CODE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
                FOR_EACH_BYTECODE_SIMD_REPLACE_LANE_OP(GENERATE_SIMD_REPLACE_LANE_CODE_CASE)
#undef GENERATE_SIMD_REPLACE_LANE_CODE_CASE
                {
                    offsets[0] = reinterpret_cast<Walrus::SIMDReplaceLane *>(byteCode)->dstOffset();
                    offsets[1] = reinterpret_cast<Walrus::SIMDReplaceLane *>(byteCode)->srcOffsets()[0];
                    offsets[2] = reinterpret_cast<Walrus::SIMDReplaceLane *>(byteCode)->srcOffsets()[1];
                    break;
                }
            case Walrus::ByteCode::SelectOpcode: {
                offsets[0] = reinterpret_cast<Walrus::Select *>(byteCode)->dstOffset();
                offsets[1] = reinterpret_cast<Walrus::Select *>(byteCode)->src0Offset();
                offsets[2] = reinterpret_cast<Walrus::Select *>(byteCode)->src1Offset();
                offsets[3] = reinterpret_cast<Walrus::Select *>(byteCode)->condOffset();
                break;
            }
            case Walrus::ByteCode::Const32Opcode:
            case Walrus::ByteCode::Const64Opcode:
            case Walrus::ByteCode::Const128Opcode: {
                offsets[0] = reinterpret_cast<Walrus::Const32 *>(byteCode)->dstOffset();
                break;
            }
            case Walrus::ByteCode::MemorySizeOpcode: {
                offsets[0] = reinterpret_cast<Walrus::MemorySize *>(byteCode)->dstOffset();
                break;
            }
            case Walrus::ByteCode::MemoryGrowOpcode: {
                offsets[0] = reinterpret_cast<Walrus::MemoryGrow *>(byteCode)->dstOffset();
                offsets[1] = reinterpret_cast<Walrus::MemoryGrow *>(byteCode)->srcOffset();
                break;
            }
            case Walrus::ByteCode::MemoryInitOpcode: {
                offsets[1] = reinterpret_cast<Walrus::MemoryInit *>(byteCode)->srcOffsets()[0];
                offsets[2] = reinterpret_cast<Walrus::MemoryInit *>(byteCode)->srcOffsets()[1];
                offsets[3] = reinterpret_cast<Walrus::MemoryInit *>(byteCode)->srcOffsets()[2];
                break;
            }
            case Walrus::ByteCode::MemoryCopyOpcode: {
                offsets[1] = reinterpret_cast<Walrus::MemoryCopy *>(byteCode)->srcOffsets()[0];
                offsets[2] = reinterpret_cast<Walrus::MemoryCopy *>(byteCode)->srcOffsets()[1];
                offsets[3] = reinterpret_cast<Walrus::MemoryCopy *>(byteCode)->srcOffsets()[2];
                break;
            }
            case Walrus::ByteCode::MemoryFillOpcode: {
                offsets[1] = reinterpret_cast<Walrus::MemoryFill *>(byteCode)->srcOffsets()[0];
                offsets[2] = reinterpret_cast<Walrus::MemoryFill *>(byteCode)->srcOffsets()[1];
                offsets[3] = reinterpret_cast<Walrus::MemoryFill *>(byteCode)->srcOffsets()[2];
                break;
            }
            case Walrus::ByteCode::RefFuncOpcode: {
                offsets[0] = reinterpret_cast<Walrus::RefFunc *>(byteCode)->dstOffset();
                break;
            }
            case Walrus::ByteCode::TableSizeOpcode: {
                offsets[0] = reinterpret_cast<Walrus::TableSize *>(byteCode)->dstOffset();
                break;
            }
            case Walrus::ByteCode::TableGrowOpcode: {
                offsets[0] = reinterpret_cast<Walrus::TableGrow *>(byteCode)->dstOffset();
                offsets[1] = reinterpret_cast<Walrus::TableGrow *>(byteCode)->src0Offset();
                offsets[2] = reinterpret_cast<Walrus::TableGrow *>(byteCode)->src1Offset();
                break;
            }
            case Walrus::ByteCode::TableGetOpcode: {
                offsets[0] = reinterpret_cast<Walrus::TableGet *>(byteCode)->dstOffset();
                offsets[1] = reinterpret_cast<Walrus::TableGet *>(byteCode)->srcOffset();
                break;
            }
            case Walrus::ByteCode::TableSetOpcode: {
                offsets[1] = reinterpret_cast<Walrus::TableSet *>(byteCode)->src0Offset();
                offsets[2] = reinterpret_cast<Walrus::TableSet *>(byteCode)->src1Offset();
                break;
            }
            case Walrus::ByteCode::TableInitOpcode: {
                offsets[1] = reinterpret_cast<Walrus::TableInit *>(byteCode)->srcOffsets()[0];
                offsets[2] = reinterpret_cast<Walrus::TableInit *>(byteCode)->srcOffsets()[1];
                offsets[3] = reinterpret_cast<Walrus::TableInit *>(byteCode)->srcOffsets()[2];
                break;
            }
            case Walrus::ByteCode::TableCopyOpcode: {
                offsets[1] = reinterpret_cast<Walrus::TableCopy *>(byteCode)->srcOffsets()[0];
                offsets[2] = reinterpret_cast<Walrus::TableCopy *>(byteCode)->srcOffsets()[1];
                offsets[3] = reinterpret_cast<Walrus::TableCopy *>(byteCode)->srcOffsets()[2];
                break;
            }
            case Walrus::ByteCode::TableFillOpcode: {
                offsets[1] = reinterpret_cast<Walrus::TableFill *>(byteCode)->srcOffsets()[0];
                offsets[2] = reinterpret_cast<Walrus::TableFill *>(byteCode)->srcOffsets()[1];
                offsets[3] = reinterpret_cast<Walrus::TableFill *>(byteCode)->srcOffsets()[2];
                break;
            }
            case Walrus::ByteCode::I8X16ShuffleOpcode: {
                offsets[0] = reinterpret_cast<Walrus::I8X16Shuffle *>(byteCode)->dstOffset();
                offsets[1] = reinterpret_cast<Walrus::I8X16Shuffle *>(byteCode)->srcOffsets()[0];
                offsets[2] = reinterpret_cast<Walrus::I8X16Shuffle *>(byteCode)->srcOffsets()[1];
                break;
            }
            case Walrus::ByteCode::V128BitSelectOpcode: {
                offsets[0] = reinterpret_cast<Walrus::V128BitSelect *>(byteCode)->dstOffset();
                offsets[1] = reinterpret_cast<Walrus::V128BitSelect *>(byteCode)->srcOffsets()[0];
                offsets[2] = reinterpret_cast<Walrus::V128BitSelect *>(byteCode)->srcOffsets()[1];
                offsets[3] = reinterpret_cast<Walrus::V128BitSelect *>(byteCode)->srcOffsets()[2];
                break;
            }
            case Walrus::ByteCode::JumpOpcode: {
                if (i != i + reinterpret_cast<Walrus::Jump *>(byteCode)->offset()) {
                    labels.emplace(i, i + reinterpret_cast<Walrus::Jump *>(byteCode)->offset());
                }
                break;
            }
            case Walrus::ByteCode::JumpIfTrueOpcode:
            case Walrus::ByteCode::JumpIfFalseOpcode: {
                offsets[1] = reinterpret_cast<Walrus::JumpIfTrue *>(byteCode)->srcOffset();

                if (i != i + reinterpret_cast<Walrus::JumpIfTrue *>(byteCode)->offset()) {
                    labels.emplace(i, i + reinterpret_cast<Walrus::JumpIfTrue *>(byteCode)->offset());
                }
                break;
            }
                // Naive range findig for variable size opcodes.
            case Walrus::ByteCode::EndOpcode: {
                ends.push_back(reinterpret_cast<Walrus::End *>(byteCode));

                Walrus::End *end = reinterpret_cast<Walrus::End *>(byteCode);
                size_t offset = 0;
                for (size_t j = 0; j < m_currentFunctionType->result().size(); j++) {
                    if (variableRange.find(end->resultOffsets()[offset]) != variableRange.end()) {
                        variableRange[end->resultOffsets()[offset]].end = i;
                        variableRange[end->resultOffsets()[offset]].gets.push_back(i);

                        if (variableRange[end->resultOffsets()[offset]].start > i) {
                            variableRange[end->resultOffsets()[offset]].start = i;
                        }
                    }
                    offset += arrayOffsetFromValue(m_currentFunctionType->result()[j]);
                }
                break;
            }
            case Walrus::ByteCode::CallOpcode:
            case Walrus::ByteCode::CallIndirectOpcode: {
                size_t params = 0;
                std::vector<Walrus::Value::Type> paramTypes;
                size_t results = 0;
                std::vector<Walrus::Value::Type> resultTypes;
                Walrus::ByteCodeStackOffset *stackOffsets = nullptr;

                if (byteCode->opcode() == Walrus::ByteCode::CallOpcode) {
                    Walrus::Call *call = reinterpret_cast<Walrus::Call *>(byteCode);
                    calls.push_back(call);
                    Walrus::ModuleFunction *target = m_result.m_functions[call->index()];
                    params = target->m_functionType->param().size();
                    results = target->m_functionType->result().size();
                    stackOffsets = call->stackOffsets();

                    std::copy(target->m_functionType->param().begin(), target->m_functionType->param().end(), std::back_inserter(paramTypes));
                    std::copy(target->m_functionType->result().begin(), target->m_functionType->result().end(), std::back_inserter(resultTypes));
                } else {
                    Walrus::CallIndirect *call = reinterpret_cast<Walrus::CallIndirect *>(byteCode);
                    callIndirects.push_back(call);
                    params = call->functionType()->param().size();
                    results = call->functionType()->result().size();
                    stackOffsets = call->stackOffsets();

                    std::copy(call->functionType()->param().begin(), call->functionType()->param().end(), std::back_inserter(paramTypes));
                    std::copy(call->functionType()->result().begin(), call->functionType()->result().end(), std::back_inserter(resultTypes));
                }

                size_t offset = 0;
                for (size_t j = 0; j < params; j++) {
                    if (variableRange[stackOffsets[offset]].end < i) {
                        variableRange[stackOffsets[offset]].end = i;
                    }
                    if (variableRange[stackOffsets[offset]].start > i) {
                        variableRange[stackOffsets[offset]].start = i;
                    }

                    variableRange[stackOffsets[offset]].gets.push_back(i);
                    offset += arrayOffsetFromValue(paramTypes[j]);
                }

                for (size_t j = 0; j < results; j++) {
                    if (variableRange[stackOffsets[offset]].end < i) {
                        variableRange[stackOffsets[offset]].end = i;
                    }
                    if (variableRange[stackOffsets[offset]].start > i) {
                        variableRange[stackOffsets[offset]].start = i;
                    }

                    variableRange[stackOffsets[offset]].sets.push_back(i);

                    offset += arrayOffsetFromValue(resultTypes[j]);
                }

                break;
            }
            case Walrus::ByteCode::BrTableOpcode: {
                Walrus::BrTable *brTable = reinterpret_cast<Walrus::BrTable *>(byteCode);
                brTables.push_back(brTable);

                if (i != i + brTable->defaultOffset()) {
                    labels.emplace(i, i + brTable->defaultOffset());
                }

                for (size_t j = 0; j < brTable->tableSize(); j++) {
                    int32_t o = brTable->jumpOffsets()[j];
                    if (i != i + o) {
                        labels.emplace(i, i + o);
                    }
                }

                if (variableRange.find(brTable->condOffset()) != variableRange.end()) {
                    if (variableRange[brTable->condOffset()].start > i) {
                        variableRange[brTable->condOffset()].start = i;
                    }
                    if (variableRange[brTable->condOffset()].end < i) {
                        variableRange[brTable->condOffset()].end = i;
                    }
                    variableRange[brTable->condOffset()].gets.push_back(i);
                }

                break;
            }
            case Walrus::ByteCode::ThrowOpcode: {
                Walrus::Throw *throwCode = reinterpret_cast<Walrus::Throw *>(byteCode);
                throws.push_back(throwCode);

                for (size_t j = 0; j < throwCode->offsetsSize(); j++) {
                    if (variableRange.find(throwCode->dataOffsets()[j]) != variableRange.end()) {
                        if (variableRange[throwCode->dataOffsets()[j]].start > i) {
                            variableRange[throwCode->dataOffsets()[j]].start = i;
                        }
                        if (variableRange[throwCode->dataOffsets()[j]].end < i) {
                            variableRange[throwCode->dataOffsets()[j]].start = i;
                        }

                        variableRange[throwCode->dataOffsets()[j]].gets.push_back(i);
                    }
                }

                break;
            }
            default: {
                i += byteCode->getSize();
                continue;
            }
            }

            // TODO: Eljárásba kiszervezni vagy valamilyen map-et használni rá amit clear-elek utána.
            if (offsets[0] == SIZE_MAX && offsets[1] == SIZE_MAX && offsets[2] == SIZE_MAX && offsets[3] == SIZE_MAX) {
                i += byteCode->getSize();
                continue;
            }


            for (auto &offset : offsets) {
                if (offset == SIZE_MAX) {
                    continue;
                }

                if (variableRange.find(offset) != variableRange.end()) {
                    if (variableRange[offset].start > i) {
                        variableRange[offset].start = i;
                    }
                    if (variableRange[offset].end < i) {
                        variableRange[offset].end = i;
                    }

                    if (offset == offsets[0]) {
                        variableRange[offset].sets.push_back(i);
                    } else {
                        variableRange[offset].gets.push_back(i);
                    }

                    offset = SIZE_MAX;
                }
            }

            // TODO: Key: offset Val: local index, keresés miatt.
            i += byteCode->getSize();
        }

        // TODO: Mondatokká alakítani.
        // End of information gathering and naive range finding. Beginning of actual analysis.

        // hasmap with own comparator func to speed things up, less will be greater with reverse ordering
        // also reuse sets in a hashmap
        // label uses a boolean to mark if it was checked, labels do not need to store their own position

        // The first elem is the set position the second tells if the set is inside a block.

        // Given any get position if bytecode offset 0 is reachable
        // without encountering any sets then the variable needs to be cleared.

        std::vector<std::pair<size_t, size_t>> seenLabels;
        for (auto &variable : variableRange) {
            // Range finding.
            // variable.second.sets.clear();

            // Bytecode induljon 1-től és alapból legyen 0 a default érték amely a nem megtaláltot jelzi.
            // Vector stackként amire pakolom az elemeket amit eredetileg getpos elemei vannak i
            // és amikor találkozok egy nem feldolgozott label-el akkor rápakolom azokat a poziciókat ahova lehet onnan ugrani.
            // Mindig amikor rápakolok egy értéket erre stackre akkor kihúzom az elemmel a range-t.
            std::vector<size_t> positions;
            std::copy(variable.second.gets.begin(), variable.second.gets.end(), std::back_inserter(positions));

            size_t pos = 0;
            while (!positions.empty()) {
                pos = positions.back();
                positions.pop_back();

                auto closestLabel = std::lower_bound(
                    labels.begin(),
                    labels.end(),
                    pos,
                    [](std::pair<size_t, size_t> elem, size_t value) {
                        return elem.first >= value;
                    });

                auto closestSet = std::lower_bound(
                    variable.second.sets.begin(),
                    variable.second.sets.end(),
                    pos,
                    [](size_t elem, size_t value) {
                        return elem >= value;
                    });

                if (closestSet == variable.second.sets.end()) {
                    variable.second.start = 0;
                    variable.second.needsInit = true;
                    continue;
                }

                if (closestLabel == labels.end()) {
                    continue;
                }

                seenLabels.push_back({ closestLabel->first, closestLabel->second });

                // both have values

                if (closestLabel->first > closestLabel->second) { // Backward jump case.
                    if (*closestSet > closestLabel->first) {
                        continue;
                    }

                    if (*closestSet < closestLabel->second) {
                        positions.push_back(closestLabel->second - 1);
                    }

                    if (*closestSet > closestLabel->second && *closestSet < closestLabel->first) {
                        positions.push_back(closestLabel->first - 1);
                    }
                } else if (closestLabel->first < closestLabel->second) { // Forward jump case.
                    if (*closestSet > closestLabel->second) {
                        continue;
                    }

                    if (*closestSet < closestLabel->first) {
                        positions.push_back(closestLabel->first - 1);
                    }

                    if (*closestSet > closestLabel->first && *closestSet < closestLabel->second) {
                        positions.push_back(closestLabel->first - 1);
                    }
                }
            }

            for (auto &label : seenLabels) {
                if (label.first < variable.second.start) {
                    variable.second.start = label.first;
                }
                if (label.second > variable.second.end) {
                    variable.second.end = label.second;
                }
            }
            seenLabels.clear();

            variable.second.gets.clear();
            variable.second.sets.clear();
        }
        labels.clear();

        // Allocation of variables on stack.

        // Ez nem biztos hogy a tényleges paramStackSize()-t adja vissza mert lehet + utolsóparamsize()
        // TODO: Paraméterek "preallocationje" azért hogy a lyukakat feltölthessük és miután a paraméterek "élete" lejárt akkor felszabadítani őket.

        // Felcserélni a vector és a listát, vectorban a slot-ok és listában a szabad elemek. A stackElement-eknek csak next
        // és prev-je van és a követező indexű elemek tárolják hogy reserved vagy connected.

        // Malloc és a free kiszervezése külön függvényben. MallocToOffset, Malloc, MallocFree.

        // Mindig amikor leallokálok egy slot-ot akkor megnézem hogy az-e a max offset

        Walrus::ByteCodeStackOffset offset = 0; // m_currentFunctionType->paramStackSize();
        std::vector<stackElement *> slots;
        stackElement *free32Slots = nullptr;
        stackElement *free64Slots = nullptr;
        stackElement *free128Slots = nullptr;


        preallocateParameters(slots, variableRange);

        offset = m_currentFunctionType->paramStackSize();

        // Position to store unused elements at.
        // TODO: Possible optimization: life cycle for unused ranges.
        stackElement *unusedVariableElem = new stackElement({ nullptr, nullptr, offset, 0 });
        size_t unusedElemOffset = offset + 4;
        bool wasUnusedVar = false;

        // Az unused elemek bemehetnek a rendes allokátoros részbe ahol a leghosszabb elemre tágítom ki mint most.
        for (auto &elem : variableRange) {
            variableRangeInfo &range = elem.second;
            if (range.start == range.end || range.start == SIZE_MAX) {
                wasUnusedVar = true;
                range.assignedPosition = unusedVariableElem;
                // elem.second.assignedPosition = unusedVariableElem;
                // elem.second.pos = unusedVariableElem->pos;
                range.pos = unusedVariableElem->pos;
                switch (range.type) {
#if defined(WALRUS_64)
                case Walrus::Value::Type::ExternRef:
                case Walrus::Value::Type::FuncRef:
#endif
                case Walrus::Value::Type::F64:
                case Walrus::Value::Type::I64: {
                    if (unusedElemOffset - offset == 4) {
                        unusedElemOffset += 4;
                    }
                    break;
                }
                case Walrus::Value::Type::V128: {
                    if (unusedElemOffset - offset == 4) {
                        unusedElemOffset += 12;
                    } else if (unusedElemOffset - offset == 8) {
                        unusedElemOffset += 8;
                    }
                    break;
                }
                default: {
                    break;
                }
                }
            }
        }

        if (wasUnusedVar) {
            offset = unusedElemOffset;
        }

        // Preallocation of parameters.

        for (size_t i = 0; i < m_currentFunction->currentByteCodeSize();) {
            Walrus::ByteCode *byteCode = reinterpret_cast<Walrus::ByteCode *>(const_cast<uint8_t *>(m_currentFunction->byteCode() + i));

            for (auto &elem : variableRange) {
                if (elem.second.start == elem.second.end) {
                    continue;
                }

                if (elem.second.end == i) {
                    deallocateLocal(slots, elem.second, offset, free32Slots, free64Slots, free64Slots);
                }

                if (elem.second.start == i && elem.second.assignedPosition == nullptr) {
                    allocateLocal(slots, elem.second, offset, free32Slots, free64Slots, free64Slots);
                    ASSERT(elem.second.assignedPosition != nullptr)
                }
            }

            i += byteCode->getSize();
        }

        // End of allocation.

        // std::vector<Walrus::ByteCodeStackOffset> constantData = {};
        // for (auto &data : m_preprocessData.m_constantData) {
        //     constantData.push_back(offset);
        //     offset += Walrus::valueSize(data.first.type());
        //     // offset += Walrus::valueStackAllocatedSize(data.first.type());
        // }

        int64_t offsetDifference = 0;
        size_t maxOffset = 0;
        for (auto &info : m_localInfo) {
            if (info.m_position >= maxOffset) {
                maxOffset = info.m_position + valueStackAllocatedSize(info.m_valueType);
            }
        }

        for (auto &data : m_preprocessData.m_constantData) {
            if (data.second >= maxOffset) {
                maxOffset = data.second + valueStackAllocatedSize(data.first.type());
            }
        }

        if (maxOffset > offset) {
            offsetDifference = maxOffset - offset;
        }
        m_currentFunction->m_requiredStackSize = m_currentFunction->m_requiredStackSize - offsetDifference;
        m_initialFunctionStackSize = m_initialFunctionStackSize - offsetDifference;

        std::vector<size_t> offsets;
        for (size_t i = 0; i < m_currentFunction->currentByteCodeSize();) {
            Walrus::ByteCode *byteCode = reinterpret_cast<Walrus::ByteCode *>(
                const_cast<uint8_t *>(m_currentFunction->byteCode() + i));

            offsets.clear();
            switch (byteCode->opcode()) {
#define GENERATE_BINARY_CODE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
                FOR_EACH_BYTECODE_BINARY_OP(GENERATE_BINARY_CODE_CASE)
                FOR_EACH_BYTECODE_SIMD_BINARY_OP(GENERATE_BINARY_CODE_CASE)
                FOR_EACH_BYTECODE_SIMD_BINARY_SHIFT_OP(GENERATE_BINARY_CODE_CASE)
                FOR_EACH_BYTECODE_SIMD_BINARY_OTHER(GENERATE_BINARY_CODE_CASE)
                {
                    Walrus::BinaryOperation *binOp = reinterpret_cast<Walrus::BinaryOperation *>(byteCode);
                    offsets.push_back(binOp->dstOffset());
                    offsets.push_back(binOp->srcOffset()[0]);
                    offsets.push_back(binOp->srcOffset()[1]);
                    offsets.push_back(SIZE_MAX);
                    break;
                }
#undef GENERATE_BINARY_CODE_CASE
#define GENERATE_UNARY_CODE_CASE(name, ...) case Walrus::ByteCode::name##Opcode:
                FOR_EACH_BYTECODE_UNARY_OP(GENERATE_UNARY_CODE_CASE)
                FOR_EACH_BYTECODE_UNARY_OP_2(GENERATE_UNARY_CODE_CASE)
                FOR_EACH_BYTECODE_SIMD_UNARY_OP(GENERATE_UNARY_CODE_CASE)
                FOR_EACH_BYTECODE_SIMD_UNARY_CONVERT_OP(GENERATE_UNARY_CODE_CASE)
                FOR_EACH_BYTECODE_SIMD_UNARY_OTHER(GENERATE_UNARY_CODE_CASE)
#undef GENERATE_UNARY_CODE_CASE
                {
                    Walrus::UnaryOperation *unOp = reinterpret_cast<Walrus::UnaryOperation *>(byteCode);
                    offsets.push_back(unOp->dstOffset());
                    offsets.push_back(unOp->srcOffset());
                    offsets.push_back(SIZE_MAX);
                    offsets.push_back(SIZE_MAX);
                    break;
                }
            case Walrus::ByteCode::I64ReinterpretF64Opcode:
            case Walrus::ByteCode::F32ReinterpretI32Opcode:
            case Walrus::ByteCode::F64ReinterpretI64Opcode:
            case Walrus::ByteCode::I32ReinterpretF32Opcode:
            case Walrus::ByteCode::MoveI32Opcode:
            case Walrus::ByteCode::MoveF32Opcode:
            case Walrus::ByteCode::MoveI64Opcode:
            case Walrus::ByteCode::MoveF64Opcode:
            case Walrus::ByteCode::MoveV128Opcode: {
                Walrus::ByteCodeOffset2 *move = reinterpret_cast<Walrus::ByteCodeOffset2 *>(byteCode);
                offsets.push_back(move->stackOffset2());
                offsets.push_back(move->stackOffset1());
                offsets.push_back(SIZE_MAX);
                offsets.push_back(SIZE_MAX);
                break;
            }
            case Walrus::ByteCode::MemorySizeOpcode: {
                offsets.push_back(reinterpret_cast<Walrus::MemorySize *>(byteCode)->dstOffset());
                offsets.push_back(SIZE_MAX);
                offsets.push_back(SIZE_MAX);
                offsets.push_back(SIZE_MAX);
                break;
            }
            case Walrus::ByteCode::MemoryInitOpcode: {
                Walrus::MemoryInit *memInit = reinterpret_cast<Walrus::MemoryInit *>(byteCode);
                offsets.push_back(SIZE_MAX);
                offsets.push_back(memInit->srcOffsets()[0]);
                offsets.push_back(memInit->srcOffsets()[1]);
                offsets.push_back(memInit->srcOffsets()[2]);
                break;
            }
            case Walrus::ByteCode::MemoryCopyOpcode: {
                Walrus::MemoryCopy *memCopy = reinterpret_cast<Walrus::MemoryCopy *>(byteCode);
                offsets.push_back(SIZE_MAX);
                offsets.push_back(memCopy->srcOffsets()[0]);
                offsets.push_back(memCopy->srcOffsets()[1]);
                offsets.push_back(memCopy->srcOffsets()[2]);
                break;
            }
            case Walrus::ByteCode::MemoryFillOpcode: {
                Walrus::MemoryFill *memFill = reinterpret_cast<Walrus::MemoryFill *>(byteCode);
                offsets.push_back(SIZE_MAX);
                offsets.push_back(memFill->srcOffsets()[0]);
                offsets.push_back(memFill->srcOffsets()[1]);
                offsets.push_back(memFill->srcOffsets()[2]);
                break;
            }
            case Walrus::ByteCode::MemoryGrowOpcode: {
                Walrus::MemoryGrow *memGrow = reinterpret_cast<Walrus::MemoryGrow *>(byteCode);
                offsets.push_back(memGrow->dstOffset());
                offsets.push_back(memGrow->srcOffset());
                offsets.push_back(SIZE_MAX);
                offsets.push_back(SIZE_MAX);
                break;
            }
            case Walrus::ByteCode::GlobalGet32Opcode:
            case Walrus::ByteCode::GlobalGet64Opcode:
            case Walrus::ByteCode::GlobalGet128Opcode: {
                Walrus::GlobalGet32 *get = reinterpret_cast<Walrus::GlobalGet32 *>(byteCode);
                offsets.push_back(get->dstOffset());
                offsets.push_back(SIZE_MAX);
                offsets.push_back(SIZE_MAX);
                offsets.push_back(SIZE_MAX);
                break;
            }
            case Walrus::ByteCode::GlobalSet32Opcode:
            case Walrus::ByteCode::GlobalSet64Opcode:
            case Walrus::ByteCode::GlobalSet128Opcode: {
                Walrus::GlobalSet32 *globalSet = reinterpret_cast<Walrus::GlobalSet32 *>(byteCode);
                offsets.push_back(SIZE_MAX);
                offsets.push_back(globalSet->srcOffset());
                offsets.push_back(SIZE_MAX);
                offsets.push_back(SIZE_MAX);
                break;
            }
            case Walrus::ByteCode::V128Load32ZeroOpcode:
            case Walrus::ByteCode::V128Load64ZeroOpcode:
#define GENERATE_MEMORY_LOAD_CODE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
                FOR_EACH_BYTECODE_LOAD_OP(GENERATE_MEMORY_LOAD_CODE_CASE)
                FOR_EACH_BYTECODE_SIMD_LOAD_EXTEND_OP(GENERATE_MEMORY_LOAD_CODE_CASE)
                FOR_EACH_BYTECODE_SIMD_LOAD_SPLAT_OP(GENERATE_MEMORY_LOAD_CODE_CASE)
#undef GENERATE_MEMORY_LOAD_CODE_CASE
                {
                    Walrus::MemoryLoad *load = reinterpret_cast<Walrus::MemoryLoad *>(byteCode);
                    offsets.push_back(load->dstOffset());
                    offsets.push_back(load->srcOffset());
                    offsets.push_back(SIZE_MAX);
                    offsets.push_back(SIZE_MAX);
                    break;
                }
#define GENERATE_SIMD_MEMORY_LOAD_LANE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
                FOR_EACH_BYTECODE_SIMD_LOAD_LANE_OP(GENERATE_SIMD_MEMORY_LOAD_LANE_CASE)
#undef GENERATE_SIMD_MEMORY_LOAD_LANE_CASE
                {
                    Walrus::SIMDMemoryLoad *load = reinterpret_cast<Walrus::SIMDMemoryLoad *>(byteCode);
                    offsets.push_back(load->dstOffset());
                    offsets.push_back(load->src0Offset());
                    offsets.push_back(load->src1Offset());
                    offsets.push_back(SIZE_MAX);
                    break;
                }
#define GENERATE_SIMD_MEMORY_STORE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
                FOR_EACH_BYTECODE_SIMD_STORE_LANE_OP(GENERATE_SIMD_MEMORY_STORE_CASE)
#undef GENERATE_SIMD_MEMORY_STORE_CASE
                {
                    Walrus::SIMDMemoryStore *store = reinterpret_cast<Walrus::SIMDMemoryStore *>(byteCode);
                    offsets.push_back(SIZE_MAX);
                    offsets.push_back(store->src0Offset());
                    offsets.push_back(store->src1Offset());
                    offsets.push_back(SIZE_MAX);
                    break;
                }
#define GENERATE_SIMD_REPLACE_LANE_CODE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
                FOR_EACH_BYTECODE_SIMD_REPLACE_LANE_OP(GENERATE_SIMD_REPLACE_LANE_CODE_CASE)
#undef GENERATE_SIMD_REPLACE_LANE_CODE_CASE
                {
                    Walrus::SIMDReplaceLane *lane = reinterpret_cast<Walrus::SIMDReplaceLane *>(byteCode);
                    offsets.push_back(lane->dstOffset());
                    offsets.push_back(lane->srcOffsets()[0]);
                    offsets.push_back(lane->srcOffsets()[1]);
                    offsets.push_back(SIZE_MAX);
                    break;
                }
#define GENERATE_SIMD_EXTRACT_LANE_CODE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
                FOR_EACH_BYTECODE_SIMD_EXTRACT_LANE_OP(GENERATE_SIMD_EXTRACT_LANE_CODE_CASE)
#undef GENERATE_SIMD_EXTRACT_LANE_CODE_CASE
                {
                    Walrus::SIMDExtractLane *lane = reinterpret_cast<Walrus::SIMDExtractLane *>(byteCode);
                    offsets.push_back(lane->dstOffset());
                    offsets.push_back(lane->srcOffset());
                    offsets.push_back(SIZE_MAX);
                    offsets.push_back(SIZE_MAX);
                    break;
                }
#define GENERATE_MEMORY_STORE_CODE_CASE(name, ...) \
    case Walrus::ByteCode::name##Opcode:
                FOR_EACH_BYTECODE_STORE_OP(GENERATE_MEMORY_STORE_CODE_CASE)
#undef GENERATE_MEMORY_STORE_CODE_CASE
                {
                    Walrus::MemoryStore *store = reinterpret_cast<Walrus::MemoryStore *>(byteCode);
                    offsets.push_back(SIZE_MAX);
                    offsets.push_back(store->src0Offset());
                    offsets.push_back(store->src1Offset());
                    offsets.push_back(SIZE_MAX);
                    break;
                }
            case Walrus::ByteCode::Load32Opcode:
            case Walrus::ByteCode::Load64Opcode: {
                Walrus::Load32 *load = reinterpret_cast<Walrus::Load32 *>(byteCode);
                offsets.push_back(load->dstOffset());
                offsets.push_back(load->srcOffset());
                offsets.push_back(SIZE_MAX);
                offsets.push_back(SIZE_MAX);
                break;
            }
            case Walrus::ByteCode::Store32Opcode:
            case Walrus::ByteCode::Store64Opcode: {
                Walrus::Store32 *store = reinterpret_cast<Walrus::Store32 *>(byteCode);
                offsets.push_back(SIZE_MAX);
                offsets.push_back(store->src0Offset());
                offsets.push_back(store->src1Offset());
                offsets.push_back(SIZE_MAX);
                break;
            }
            case Walrus::ByteCode::SelectOpcode: {
                Walrus::Select *select = reinterpret_cast<Walrus::Select *>(byteCode);
                offsets.push_back(select->dstOffset());
                offsets.push_back(select->src0Offset());
                offsets.push_back(select->src1Offset());
                offsets.push_back(select->condOffset());
                break;
            }
            case Walrus::ByteCode::Const32Opcode:
            case Walrus::ByteCode::Const64Opcode:
            case Walrus::ByteCode::Const128Opcode: {
                Walrus::Const32 *constant = reinterpret_cast<Walrus::Const32 *>(byteCode);
                offsets.push_back(constant->dstOffset());
                offsets.push_back(SIZE_MAX);
                offsets.push_back(SIZE_MAX);
                offsets.push_back(SIZE_MAX);
                break;
            }
            case Walrus::ByteCode::JumpIfTrueOpcode:
            case Walrus::ByteCode::JumpIfFalseOpcode: {
                Walrus::JumpIfFalse *jump = reinterpret_cast<Walrus::JumpIfFalse *>(byteCode);
                offsets.push_back(SIZE_MAX);
                offsets.push_back(jump->srcOffset());
                offsets.push_back(SIZE_MAX);
                offsets.push_back(SIZE_MAX);
                break;
            }
            case Walrus::ByteCode::RefFuncOpcode: {
                offsets.push_back(reinterpret_cast<Walrus::RefFunc *>(byteCode)->dstOffset());
                offsets.push_back(SIZE_MAX);
                offsets.push_back(SIZE_MAX);
                offsets.push_back(SIZE_MAX);
                break;
            }
            case Walrus::ByteCode::TableSizeOpcode: {
                offsets.push_back(reinterpret_cast<Walrus::TableSize *>(byteCode)->dstOffset());
                offsets.push_back(SIZE_MAX);
                offsets.push_back(SIZE_MAX);
                offsets.push_back(SIZE_MAX);
                break;
            }
            case Walrus::ByteCode::TableGrowOpcode: {
                offsets.push_back(reinterpret_cast<Walrus::TableGrow *>(byteCode)->dstOffset());
                offsets.push_back(reinterpret_cast<Walrus::TableGrow *>(byteCode)->src0Offset());
                offsets.push_back(reinterpret_cast<Walrus::TableGrow *>(byteCode)->src1Offset());
                offsets.push_back(SIZE_MAX);
                break;
            }
            case Walrus::ByteCode::TableGetOpcode: {
                offsets.push_back(reinterpret_cast<Walrus::TableGet *>(byteCode)->dstOffset());
                offsets.push_back(reinterpret_cast<Walrus::TableGet *>(byteCode)->srcOffset());
                offsets.push_back(SIZE_MAX);
                offsets.push_back(SIZE_MAX);
                break;
            }
            case Walrus::ByteCode::TableSetOpcode: {
                offsets.push_back(SIZE_MAX);
                offsets.push_back(reinterpret_cast<Walrus::TableSet *>(byteCode)->src0Offset());
                offsets.push_back(reinterpret_cast<Walrus::TableSet *>(byteCode)->src1Offset());
                offsets.push_back(SIZE_MAX);
                break;
            }
            case Walrus::ByteCode::TableInitOpcode: {
                offsets.push_back(SIZE_MAX);
                offsets.push_back(reinterpret_cast<Walrus::TableInit *>(byteCode)->srcOffsets()[0]);
                offsets.push_back(reinterpret_cast<Walrus::TableInit *>(byteCode)->srcOffsets()[1]);
                offsets.push_back(reinterpret_cast<Walrus::TableInit *>(byteCode)->srcOffsets()[2]);
                break;
            }
            case Walrus::ByteCode::TableCopyOpcode: {
                offsets.push_back(SIZE_MAX);
                offsets.push_back(reinterpret_cast<Walrus::TableCopy *>(byteCode)->srcOffsets()[0]);
                offsets.push_back(reinterpret_cast<Walrus::TableCopy *>(byteCode)->srcOffsets()[1]);
                offsets.push_back(reinterpret_cast<Walrus::TableCopy *>(byteCode)->srcOffsets()[2]);
                break;
            }
            case Walrus::ByteCode::TableFillOpcode: {
                offsets.push_back(SIZE_MAX);
                offsets.push_back(reinterpret_cast<Walrus::TableFill *>(byteCode)->srcOffsets()[0]);
                offsets.push_back(reinterpret_cast<Walrus::TableFill *>(byteCode)->srcOffsets()[1]);
                offsets.push_back(reinterpret_cast<Walrus::TableFill *>(byteCode)->srcOffsets()[2]);
                break;
            }
            case Walrus::ByteCode::I8X16ShuffleOpcode: {
                offsets.push_back(reinterpret_cast<Walrus::I8X16Shuffle *>(byteCode)->dstOffset());
                offsets.push_back(reinterpret_cast<Walrus::I8X16Shuffle *>(byteCode)->srcOffsets()[0]);
                offsets.push_back(reinterpret_cast<Walrus::I8X16Shuffle *>(byteCode)->srcOffsets()[1]);
                offsets.push_back(SIZE_MAX);
                break;
            }
            case Walrus::ByteCode::V128BitSelectOpcode: {
                offsets.push_back(reinterpret_cast<Walrus::V128BitSelect *>(byteCode)->dstOffset());
                offsets.push_back(reinterpret_cast<Walrus::V128BitSelect *>(byteCode)->srcOffsets()[0]);
                offsets.push_back(reinterpret_cast<Walrus::V128BitSelect *>(byteCode)->srcOffsets()[1]);
                offsets.push_back(reinterpret_cast<Walrus::V128BitSelect *>(byteCode)->srcOffsets()[2]);
                break;
            }
            default: {
                i += byteCode->getSize();
                continue;
            }
            }

            for (size_t j = 0; j < m_currentFunctionType->param().size(); j++) {
                if (offsets[0] == m_localInfo[j].m_position) {
                    setByteCodeDestination(byteCode, variableRange[m_localInfo[j].m_position].pos);
                    offsets[0] = SIZE_MAX;
                }
                if (offsets[1] == m_localInfo[j].m_position) {
                    setByteCodeSrc0(byteCode, variableRange[m_localInfo[j].m_position].pos);
                    offsets[1] = SIZE_MAX;
                }
                if (offsets[2] == m_localInfo[j].m_position) {
                    setByteCodeSrc1(byteCode, variableRange[m_localInfo[j].m_position].pos);
                    offsets[2] = SIZE_MAX;
                }
                if (offsets[3] == m_localInfo[j].m_position) {
                    setByteCodeExtra(byteCode, variableRange[m_localInfo[j].m_position].pos);
                    offsets[3] = SIZE_MAX;
                }
            }

            for (size_t j = 0; j < m_preprocessData.m_constantData.size(); j++) {
                if (offsets[0] == m_preprocessData.m_constantData[j].second) {
                    setByteCodeDestination(byteCode, variableRange[m_preprocessData.m_constantData[j].second].pos);
                    offsets[0] = SIZE_MAX;
                }
                if (offsets[1] == m_preprocessData.m_constantData[j].second) {
                    setByteCodeSrc0(byteCode, variableRange[m_preprocessData.m_constantData[j].second].pos);
                    offsets[1] = SIZE_MAX;
                }
                if (offsets[2] == m_preprocessData.m_constantData[j].second) {
                    setByteCodeSrc1(byteCode, variableRange[m_preprocessData.m_constantData[j].second].pos);
                    offsets[2] = SIZE_MAX;
                }
                if (offsets[3] == m_preprocessData.m_constantData[j].second) {
                    setByteCodeExtra(byteCode, variableRange[m_preprocessData.m_constantData[j].second].pos);
                    offsets[3] = SIZE_MAX;
                }
            }

            for (size_t k = 0; k < offsets.size(); k++) {
                bool local = false;
                for (size_t j = m_currentFunctionType->param().size(); j < m_localInfo.size(); j++) {
                    if (offsets[k] == m_localInfo[j].m_position) {
                        local = true;
                    }
                }

                if (!local && offsets[k] != SIZE_MAX && (int64_t)(offsets[k] - offsetDifference) >= 0) {
                    // stackValue
                    switch (k) {
                    case 0: {
                        setByteCodeDestination(byteCode, offsets[0] - offsetDifference);
                        break;
                    }
                    case 1: {
                        setByteCodeSrc0(byteCode, offsets[1] - offsetDifference);
                        break;
                    }
                    case 2: {
                        setByteCodeSrc1(byteCode, offsets[2] - offsetDifference);
                        break;
                    }
                    case 3: {
                        setByteCodeExtra(byteCode, offsets[3] - offsetDifference);
                        break;
                    }
                    default: {
                        RELEASE_ASSERT_NOT_REACHED();
                        break;
                    }
                    }
                }

                if (!local) {
                    offsets[k] = SIZE_MAX;
                }
            }

            bool noLocals = true;
            for (auto &offset : offsets) {
                if (offset != SIZE_MAX) {
                    noLocals = false;
                }
            }

            if (noLocals) {
                i += byteCode->getSize();
                continue;
            }

            for (auto &range : variableRange) {
                if (range.second.originalPosition == offsets[0]) {
                    setByteCodeDestination(byteCode, range.second.pos);
                }

                if (range.second.originalPosition == offsets[1]) {
                    setByteCodeSrc0(byteCode, range.second.pos);
                }

                if (range.second.originalPosition == offsets[2]) {
                    setByteCodeSrc1(byteCode, range.second.pos);
                }

                if (range.second.originalPosition == offsets[3]) {
                    setByteCodeExtra(byteCode, range.second.pos);
                }
            }

            i += byteCode->getSize();
        }


        {
            std::vector<size_t> sizes;
            std::vector<Walrus::ByteCodeStackOffset *> stackOffsets;

            for (auto &end : ends) {
                sizes.push_back(end->offsetsSize());
                stackOffsets.push_back(end->resultOffsets());
            }

            for (auto &call : calls) {
                sizes.push_back(call->parameterOffsetsSize() + call->resultOffsetsSize());
                stackOffsets.push_back(call->stackOffsets());
            }

            for (auto &call : callIndirects) {
                sizes.push_back(call->parameterOffsetsSize() + call->resultOffsetsSize());
                stackOffsets.push_back(call->stackOffsets());

                bool found = false;
                for (uint32_t constantIdx = 0; constantIdx < m_preprocessData.m_constantData.size() && !found; constantIdx++) {
                    if (m_preprocessData.m_constantData[constantIdx].second == call->calleeOffset()) {
                        call->setCalleeOffset(variableRange[m_preprocessData.m_constantData[constantIdx].second].pos);
                        found = true;
                    }
                }

                for (auto it = variableRange.begin(); it != variableRange.end() && !found; it++) {
                    if (it->second.originalPosition == call->calleeOffset()) {
                        call->setCalleeOffset(it->second.pos);
                        found = true;
                    }
                }

                if (!found) {
                    call->setCalleeOffset(call->calleeOffset() - offsetDifference);
                }
            }

            for (auto &throwCode : throws) {
                sizes.push_back(throwCode->offsetsSize());
                stackOffsets.push_back(throwCode->dataOffsets());
            }

            for (uint32_t stackOffsetIndex = 0; stackOffsetIndex < stackOffsets.size(); stackOffsetIndex++) {
                for (uint32_t offsetIndex = 0; offsetIndex < sizes[stackOffsetIndex]; offsetIndex++) {
                    bool constant = false;
                    for (uint32_t constantIndex = 0; constantIndex < m_preprocessData.m_constantData.size(); constantIndex++) {
                        if (stackOffsets[stackOffsetIndex][offsetIndex] == m_preprocessData.m_constantData[constantIndex].second) {
                            stackOffsets[stackOffsetIndex][offsetIndex] = variableRange[m_preprocessData.m_constantData[constantIndex].second].pos;
                            constant = true;
                        }
                    }

                    if (constant) {
                        continue;
                    }

                    bool local = false;
                    // for (auto &range : variableRange) {
                    if (variableRange.find(stackOffsets[stackOffsetIndex][offsetIndex]) != variableRange.end()) {
                        stackOffsets[stackOffsetIndex][offsetIndex] = variableRange[stackOffsets[stackOffsetIndex][offsetIndex]].pos;
                        local = true;

#if defined(WALRUS_32)
                        switch (variableRange[stackOffsets[stackOffsetIndex][offsetIndex]].type) {
                        case Walrus::Value::I64:
                        case Walrus::Value::F64: {
                            stackOffsets[stackOffsetIndex][offsetIndex + 1] = variableRange[stackOffsets[stackOffsetIndex][offsetIndex]].pos + 4;
                            offsetindex++;
                            break;
                        }
                        case Walrus::Value::V128: {
                            for (uint32_t idx = 1; idx < 4; idx++) {
                                offsetIndex;
                                stackOffsets[stackOffsetIndex][offsetIndex] = variableRange[stackOffsets[stackOffsetIndex][offsetIndex]].pos + (idx * 4);
                            }
                            break;
                        }
                        default: {
                            break;
                        }
                        }
#else
                        if (variableRange[stackOffsets[stackOffsetIndex][offsetIndex]].type == Walrus::Value::V128) {
                            stackOffsets[stackOffsetIndex][offsetIndex + 1] = variableRange[stackOffsets[stackOffsetIndex][offsetIndex]].pos + 8;
                            break;
                        }
#endif
                    }
                    //}

                    if (local) {
                        continue;
                    }

                    stackOffsets[stackOffsetIndex][offsetIndex] = stackOffsets[stackOffsetIndex][offsetIndex] - offsetDifference;
                }
            }
        }

        for (auto &brTable : brTables) {
            for (auto &range : variableRange) {
                bool local = false;
                bool constant = false;

                for (size_t k = 0; k < m_preprocessData.m_constantData.size(); k++) {
                    if (m_preprocessData.m_constantData[k].second == brTable->condOffset()) {
                        constant = true;
                        brTable->setCondOffset(variableRange[m_preprocessData.m_constantData[k].second].pos);
                    }
                }

                if (!constant) {
                    if (range.second.pos == brTable->condOffset()) {
                        local = true;
                        brTable->setCondOffset(range.second.pos);
                        break;
                    }
                }

                if (!local && !constant && (int64_t)(brTable->condOffset() - offsetDifference) >= 0) {
                    brTable->setCondOffset(brTable->condOffset() - offsetDifference);
                }
            }
        }

        m_localInfo.clear();
        m_currentFunction->m_local.clear();
        for (auto &range : variableRange) {
            m_localInfo.push_back({ range.second.type, range.second.pos });
            m_currentFunction->m_local.push_back(range.second.type);
        }

        for (uint32_t j = 0; j < m_currentFunctionType->param().size(); j++) {
            if (variableRange[m_localInfo[j].m_position].needsInit) {
                variableRange[m_localInfo[j].m_position].needsInit = false;
            }
        }

        size_t constSize = 0;
        for (auto &range : variableRange) {
            for (uint32_t j = 0; j < m_currentFunctionType->param().size(); j++) {
                if (range.second.originalPosition == m_localInfo[j].m_position) {
                    continue;
                }
            }

            if (range.second.needsInit) {
                constSize = pushInitByteCodes(range.second);
            }
        }

#if !defined(NDEBUG)
        // TODO: Ellenőrizni hogy DUMP_BYTECODE env var létezik-e.


        if (!(getenv("DUMP_BYTECODE") && strlen(getenv("DUMP_BYTECODE")))) {
            return;
        }

        for (auto &range : variableRange) {
            if (range.second.start != 0 && range.second.start != SIZE_MAX) {
                range.second.start += constSize;
            }
            range.second.end += constSize;
        }

        for (auto &range : variableRange) {
            if (range.second.end != SIZE_MAX) {
                m_currentFunction->m_variabeRange.push_back({ range.second.start, range.second.end });
            }
        }

        m_currentFunction->m_localDebugData.clear();
        for (auto &range : variableRange) {
            Walrus::ModuleFunction::LocalDebugInfo debugInfo;
            debugInfo.stackPosition = range.second.pos;

            debugInfo.start = range.second.start;
            debugInfo.end = range.second.end;

            m_currentFunction->m_localDebugData.push_back(debugInfo);
        }
        m_currentFunction->m_localDebugData.push_back({ unusedVariableElem->pos, 0, m_currentFunction->currentByteCodeSize() });
        delete unusedVariableElem;

        // for (size_t i = 0; i < constantData.size(); i++) {
        //     m_currentFunction->m_constantDebugData[i].second = constantData[i];
        // }
#endif
        // variable life analysis end
    }
};

} // namespace wabt

namespace Walrus {
WASMParsingResult::WASMParsingResult()
    : m_seenStartAttribute(false)
    , m_version(0)
    , m_start(0)
{
}

void WASMParsingResult::clear()
{
    for (size_t i = 0; i < m_imports.size(); i++) {
        delete m_imports[i];
    }

    for (size_t i = 0; i < m_exports.size(); i++) {
        delete m_exports[i];
    }

    for (size_t i = 0; i < m_functions.size(); i++) {
        delete m_functions[i];
    }

    for (size_t i = 0; i < m_datas.size(); i++) {
        delete m_datas[i];
    }

    for (size_t i = 0; i < m_elements.size(); i++) {
        delete m_elements[i];
    }

    for (size_t i = 0; i < m_functionTypes.size(); i++) {
        delete m_functionTypes[i];
    }

    for (size_t i = 0; i < m_globalTypes.size(); i++) {
        delete m_globalTypes[i];
    }

    for (size_t i = 0; i < m_tableTypes.size(); i++) {
        delete m_tableTypes[i];
    }

    for (size_t i = 0; i < m_memoryTypes.size(); i++) {
        delete m_memoryTypes[i];
    }

    for (size_t i = 0; i < m_tagTypes.size(); i++) {
        delete m_tagTypes[i];
    }
}

std::pair<Optional<Module *>, std::string> WASMParser::parseBinary(Store *store, const std::string &filename, const uint8_t *data, size_t len, const uint32_t JITFlags)
{
    wabt::WASMBinaryReader delegate;

    std::string error = ReadWasmBinary(filename, data, len, &delegate);
    if (error.length()) {
        return std::make_pair(nullptr, error);
    }

    Module *module = new Module(store, delegate.parsingResult());
    if (JITFlags & JITFlagValue::useJIT) {
        module->jitCompile(nullptr, 0, JITFlags);
    }

    return std::make_pair(module, std::string());
}

} // namespace Walrus
