#pragma once
#include "Engine_classes.hpp"
#include "plugin_helpers.h"
#include <initializer_list>
#include <string>

namespace MainMapProofReflection
{
    struct Field { const char* name; int offset; int size; const char* type; uint64_t flags=0x80; };
    inline bool Require(SDK::UClass* cls, const char* owner, const char* name, int size,
        std::initializer_list<Field> fields)
    {
        auto* fn = cls ? cls->GetFunction(owner, name) : nullptr;
        if (!fn || fn->Size != size)
        {
            LOG_ERROR("MainMap A2.3: reflection gate FAILED %s.%s size=%d expected=%d",
                owner, name, fn ? fn->Size : -1, size);
            return false;
        }
        int count = 0;
        for (auto* field = fn->ChildProperties; field && count < 32; field = field->Next)
        {
            ++count;
            auto* prop = static_cast<SDK::FProperty*>(field);
            bool matched = false;
            for (const auto& expected : fields)
            {
                const uint64_t flags=std::string(expected.name)=="ReturnValue" ? 0x580 : expected.flags;
                if (field->Name.GetRawString() == expected.name && prop->Offset == expected.offset &&
                    prop->ElementSize == expected.size && prop->ArrayDim == 1 &&
                    field->ClassPrivate && field->ClassPrivate->Name.GetRawString() == expected.type &&
                    (prop->PropertyFlags & 0x8000582ULL)==flags)
                    matched = true;
            }
            if (!matched)
            {
                LOG_ERROR("MainMap A2.3: reflection field FAILED %s.%s.%s offset=%d size=%d type=%s flags=%llx",
                    owner, name, field->Name.GetRawString().c_str(), prop->Offset, prop->ElementSize,
                    field->ClassPrivate ? field->ClassPrivate->Name.GetRawString().c_str() : "null",
                    prop->PropertyFlags);
                return false;
            }
        }
        if (count != fields.size()) return false;
        LOG_INFO("MainMap A2.3: reflection layout PASS %s.%s size=%d fields=%d", owner, name, size, count);
        return true;
    }
}
