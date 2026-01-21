#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

enum class MemberKind {
    Method,
    Ctor,
    Field,
    Property,
    Event,
    EnumValue
};

struct DumpMember {
    MemberKind kind;

    std::string name;
    std::string signature;
    std::string details;
    std::string returnType;
    std::string params;
    std::string modifiers;

    uint64_t rva = 0;
    uint64_t offset = 0;
    uint64_t va = 0;

    std::string enumValue;
};

struct DumpType {
    std::string name;
    std::string nameSpace;
    int typeDefIndex = -1;
    std::string assembly;
    bool isEnum = false;

    std::vector<DumpMember> members;
};

class DumpCsParser {
public:
    static void setProgressCallback(const std::function<void(int)>& cb);
    static std::vector<DumpType> parse(const std::string& path);
};