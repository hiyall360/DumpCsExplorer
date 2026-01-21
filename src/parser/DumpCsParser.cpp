#include "DumpCsParser.h"

#include <fstream>
#include <regex>
#include <sstream>
#include <algorithm>
#include <filesystem>

static std::function<void(int)> g_progressCb = nullptr;

void DumpCsParser::setProgressCallback(const std::function<void(int)>& cb) {
    g_progressCb = cb;
}

static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, e - b + 1);
}

static uint64_t parseHex(const std::string& s) {
    return std::stoull(s, nullptr, 16);
}

static std::string stripInlineComment(const std::string& s) {
    const auto p = s.find("//");
    if (p == std::string::npos) return s;
    return trim(s.substr(0, p));
}

std::vector<DumpType> DumpCsParser::parse(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<DumpType> out;
    if (!f.is_open())
        return out;

    const std::uintmax_t totalSize =
        std::filesystem::exists(path) ? std::filesystem::file_size(path) : 0;

    std::string line;
    std::string currentNs;
    DumpType* currentType = nullptr;
    std::string section;

    struct ImageMapEntry {
        int baseTypeDefIndex = -1;
        std::string assembly;
    };
    std::vector<ImageMapEntry> images;

    bool hasPending = false;
    uint64_t pendingRva = 0, pendingOff = 0, pendingVa = 0;

    std::regex rxImage(R"(//\s*Image\s+\d+:\s*(.+?)\s*-\s*(\d+)\s*)");
    std::regex rxNamespace(R"(//\s*Namespace:\s*(.*))");
    std::regex rxType(R"((class|struct|enum|interface)\s+([^\s:{]+))");
    std::regex rxTypeDefIndex(R"(TypeDefIndex:\s*(\d+))");
    std::regex rxSection(R"(//\s*(Methods|Fields|Properties|Events))");

    std::regex rxRva(R"(RVA:\s*0x([0-9A-Fa-f]+))");
    std::regex rxOff(R"(Offset:\s*0x([0-9A-Fa-f]+))");
    std::regex rxVa (R"(VA:\s*0x([0-9A-Fa-f]+))");

    static const std::regex rxMethod(
        R"(^\s*((?:(?:public|private|protected|internal|static|virtual|override|abstract|sealed|extern)\s+)*)\s*(?:([\w<>\[\],]+)\s+)?([\w_]+)\s*\(([^)]*)\))"
    );

    std::regex rxEnumValue(R"(^\s*(?:public|private|protected|internal)?\s*(?:const\s+)?[\w<>\[\],\.]+\s+([\w_]+)\s*=\s*([^;]+))");

    std::regex rxInlineHex(R"(0x([0-9A-Fa-f]+))");

    size_t lineCount = 0;

    while (std::getline(f, line)) {
        if (++lineCount % 200 == 0 && g_progressCb && totalSize > 0) {
            const auto pos = static_cast<std::uintmax_t>(f.tellg());
            int percent = static_cast<int>((pos * 100) / totalSize);
            g_progressCb(std::clamp(percent, 0, 100));
        }

        std::string s = trim(line);
        std::smatch m;

        if (std::regex_search(s, m, rxImage)) {
            ImageMapEntry e;
            e.assembly = trim(m[1].str());
            e.baseTypeDefIndex = std::stoi(m[2].str());
            images.push_back(std::move(e));
            continue;
        }

        if (std::regex_search(s, m, rxNamespace)) {
            currentNs = m[1].str();
            if (currentNs.empty()) currentNs = "-";
            continue;
        }

        if (std::regex_search(s, m, rxType)) {
            out.push_back({});
            currentType = &out.back();
            currentType->name = m[2];
            currentType->nameSpace = currentNs.empty() ? "-" : currentNs;
            currentType->isEnum = (m[1] == "enum");

            std::smatch mt;
            if (std::regex_search(s, mt, rxTypeDefIndex)) {
                currentType->typeDefIndex = std::stoi(mt[1].str());

                std::string asmName;
                int bestBase = -1;
                for (const auto& img : images) {
                    if (img.baseTypeDefIndex <= currentType->typeDefIndex && img.baseTypeDefIndex >= bestBase) {
                        bestBase = img.baseTypeDefIndex;
                        asmName = img.assembly;
                    }
                }
                currentType->assembly = std::move(asmName);
            }

            section.clear();
            hasPending = false;
            continue;
        }

        if (!currentType)
            continue;

        if (std::regex_search(s, m, rxSection)) {
            section = m[1];
            continue;
        }

        if (section.empty())
            continue;

        if (currentType->isEnum && section == "Fields") {
            if (s.find("value__") != std::string::npos) {
                DumpMember mem;
                mem.kind = MemberKind::Field;
                mem.signature = stripInlineComment(s);
                mem.details = mem.signature;

                std::smatch mh;
                if (std::regex_search(s, mh, rxInlineHex)) {
                    mem.offset = parseHex(mh[1]);
                    std::ostringstream oss;
                    oss << mem.signature << "\n"
                        << "Offset: 0x" << std::hex << mem.offset;
                    mem.details = oss.str();
                }

                currentType->members.push_back(mem);
                continue;
            }

            if (std::regex_search(s, m, rxEnumValue)) {
                DumpMember mem;
                mem.kind = MemberKind::EnumValue;
                mem.name = m[1];
                mem.enumValue = trim(m[2]);
                mem.signature = mem.name + " = " + mem.enumValue;
                mem.details = mem.signature;
                currentType->members.push_back(mem);
            }
            continue;
        }

        if (section == "Methods") {
            if (s.rfind("//", 0) == 0 &&
                (s.find("RVA:") != std::string::npos ||
                 s.find("Offset:") != std::string::npos ||
                 s.find("VA:") != std::string::npos)) {

                std::smatch ma;
                if (std::regex_search(s, ma, rxRva)) pendingRva = parseHex(ma[1]);
                if (std::regex_search(s, ma, rxOff)) pendingOff = parseHex(ma[1]);
                if (std::regex_search(s, ma, rxVa )) pendingVa  = parseHex(ma[1]);

                hasPending = true;
                continue;
            }

            if (s.empty() || s == "{" || s == "}" || s.rfind("//", 0) == 0)
                continue;

            if (!hasPending)
                continue;

            if (!std::regex_search(s, m, rxMethod))
                continue;

            DumpMember mem;
            mem.modifiers = trim(m[1]);
            mem.returnType = trim(m[2]);
            mem.name = m[3];
            mem.params = trim(m[4]);

            if (mem.name == currentType->name) {
                mem.kind = MemberKind::Ctor;
                mem.name = ".ctor";
                mem.returnType.clear();
            } else {
                if (mem.name.rfind("get_", 0) == 0 || mem.name.rfind("set_", 0) == 0)
                    mem.kind = MemberKind::Property;
                else if (mem.name.rfind("add_", 0) == 0 || mem.name.rfind("remove_", 0) == 0)
                    mem.kind = MemberKind::Event;
                else
                    mem.kind = MemberKind::Method;
            }

            mem.signature.clear();
            if (!mem.modifiers.empty()) {
                mem.signature += mem.modifiers;
                mem.signature += " ";
            }
            if (!mem.returnType.empty()) {
                mem.signature += mem.returnType;
                mem.signature += " ";
            }
            mem.signature += mem.name + "(" + mem.params + ")";

            mem.rva = pendingRva;
            mem.offset = pendingOff ? pendingOff : pendingRva;
            mem.va = pendingVa;

            std::ostringstream oss;
            oss << mem.signature << "\n"
                << "RVA: 0x" << std::hex << mem.rva
                << "  Offset: 0x" << mem.offset
                << "  VA: 0x" << mem.va;

            mem.details = oss.str();
            currentType->members.push_back(mem);

            hasPending = false;
            pendingRva = pendingOff = pendingVa = 0;
        }

        if (section == "Fields" || section == "Properties" || section == "Events") {
            if (s.empty() || s == "{" || s == "}" || s.rfind("//", 0) == 0)
                continue;

            DumpMember mem;
            mem.kind = (section == "Fields") ? MemberKind::Field :
                       (section == "Properties") ? MemberKind::Property :
                       MemberKind::Event;

            mem.signature = stripInlineComment(s);
            mem.details = mem.signature;

            std::smatch mh;
            if (std::regex_search(s, mh, rxInlineHex)) {
                mem.offset = parseHex(mh[1]);
                std::ostringstream oss;
                oss << mem.signature << "\n"
                    << "Offset: 0x" << std::hex << mem.offset;
                mem.details = oss.str();
            }

            currentType->members.push_back(mem);
            continue;
        }
    }

    if (g_progressCb)
        g_progressCb(100);

    return out;
}
