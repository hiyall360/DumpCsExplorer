#include "DumpCsParser.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
 #include <charconv>

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

static bool tryParseHex(const std::string& s, uint64_t& out) {
    out = 0;
    const char* b = s.data();
    const char* e = s.data() + s.size();
    auto res = std::from_chars(b, e, out, 16);
    return res.ec == std::errc() && res.ptr == e;
}

static bool tryParseInt(const std::string& s, int& out) {
    out = 0;
    const char* b = s.data();
    const char* e = s.data() + s.size();
    auto res = std::from_chars(b, e, out, 10);
    return res.ec == std::errc() && res.ptr == e;
}

static std::string stripInlineComment(const std::string& s) {
    const auto p = s.find("//");
    if (p == std::string::npos) return s;
    return trim(s.substr(0, p));
}

static bool startsWith(const std::string& s, const char* prefix) {
    const size_t n = std::char_traits<char>::length(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

static bool tryExtractFirstHexAfter(const std::string& s, const char* key, uint64_t& out) {
    const auto p = s.find(key);
    if (p == std::string::npos)
        return false;
    const auto x = s.find("0x", p);
    if (x == std::string::npos)
        return false;
    size_t i = x + 2;
    size_t j = i;
    while (j < s.size()) {
        const char c = s[j];
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!ok) break;
        ++j;
    }
    if (j == i)
        return false;
    return tryParseHex(s.substr(i, j - i), out);
}

static bool tryExtractFirstInlineHex(const std::string& s, uint64_t& out) {
    const auto x = s.find("0x");
    if (x == std::string::npos)
        return false;
    size_t i = x + 2;
    size_t j = i;
    while (j < s.size()) {
        const char c = s[j];
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!ok) break;
        ++j;
    }
    if (j == i)
        return false;
    return tryParseHex(s.substr(i, j - i), out);
}

static bool tryParseImageLine(const std::string& s, std::string& assemblyOut, int& baseTypeDefIndexOut) {
    if (!startsWith(s, "//"))
        return false;
    if (s.find("Image") == std::string::npos)
        return false;
    const auto colon = s.find(':');
    if (colon == std::string::npos)
        return false;
    const auto dash = s.rfind('-');
    if (dash == std::string::npos || dash <= colon)
        return false;

    assemblyOut = trim(s.substr(colon + 1, dash - (colon + 1)));
    const std::string idxStr = trim(s.substr(dash + 1));
    int v = 0;
    if (!tryParseInt(idxStr, v))
        return false;
    baseTypeDefIndexOut = v;
    return !assemblyOut.empty();
}

static bool tryParseNamespaceLine(const std::string& s, std::string& nsOut) {
    if (!startsWith(s, "//"))
        return false;
    const auto p = s.find("Namespace:");
    if (p == std::string::npos)
        return false;
    nsOut = trim(s.substr(p + std::char_traits<char>::length("Namespace:")));
    return true;
}

static bool tryParseSectionLine(const std::string& s, std::string& sectionOut) {
    if (!startsWith(s, "//"))
        return false;
    const std::string t = trim(s.substr(2));
    if (t == "Methods" || t == "Fields" || t == "Properties" || t == "Events") {
        sectionOut = t;
        return true;
    }
    return false;
}

static int findTypeKeywordPos(const std::string& s, std::string& kwOut) {
    static const char* kws[] = {"class", "struct", "enum", "interface"};
    for (const auto* kw : kws) {
        size_t pos = 0;
        while (true) {
            pos = s.find(kw, pos);
            if (pos == std::string::npos)
                break;
            const bool leftOk = (pos == 0) || !((s[pos - 1] >= 'A' && s[pos - 1] <= 'Z') || (s[pos - 1] >= 'a' && s[pos - 1] <= 'z') || (s[pos - 1] >= '0' && s[pos - 1] <= '9') || s[pos - 1] == '_');
            const size_t end = pos + std::char_traits<char>::length(kw);
            const bool rightOk = (end < s.size()) && (s[end] == ' ' || s[end] == '\t');
            if (leftOk && rightOk) {
                kwOut = kw;
                return (int)pos;
            }
            pos = end;
        }
    }
    return -1;
}

static bool tryParseTypeLine(const std::string& s, std::string& kindOut, std::string& nameOut) {
    std::string kw;
    const int pos = findTypeKeywordPos(s, kw);
    if (pos < 0)
        return false;
    size_t i = (size_t)pos + kw.size();
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        ++i;
    if (i >= s.size())
        return false;
    size_t j = i;
    while (j < s.size()) {
        const char c = s[j];
        if (c == ' ' || c == '\t' || c == ':' || c == '{' || c == '\r' || c == '\n')
            break;
        ++j;
    }
    if (j == i)
        return false;
    kindOut = kw;
    nameOut = s.substr(i, j - i);
    return !nameOut.empty();
}

static bool tryParseTypeDefIndex(const std::string& s, int& out) {
    const auto p = s.find("TypeDefIndex:");
    if (p == std::string::npos)
        return false;
    size_t i = p + std::char_traits<char>::length("TypeDefIndex:");
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        ++i;
    size_t j = i;
    while (j < s.size() && (s[j] >= '0' && s[j] <= '9'))
        ++j;
    if (j == i)
        return false;
    return tryParseInt(s.substr(i, j - i), out);
}

static std::string parseLeadingModifiers(const std::string& leftPart, std::string& remainderOut) {
    static const char* mods[] = {
        "public","private","protected","internal","static","virtual","override","abstract","sealed","extern",
        "readonly","const","volatile","unsafe","new","partial","async","ref","out","in"
    };

    size_t i = 0;
    std::string modsOut;
    std::string s = leftPart;
    s = trim(s);
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
            ++i;
        if (i >= s.size())
            break;
        size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t')
            ++j;
        const std::string tok = s.substr(i, j - i);
        bool isMod = false;
        for (const auto* m : mods) {
            if (tok == m) {
                isMod = true;
                break;
            }
        }
        if (!isMod)
            break;
        if (!modsOut.empty())
            modsOut += " ";
        modsOut += tok;
        i = j;
    }

    remainderOut = trim(s.substr(i));
    return modsOut;
}

static bool tryParseMethodLike(const std::string& s, std::string& modifiersOut, std::string& returnTypeOut, std::string& nameOut, std::string& paramsOut) {
    const auto paren = s.find('(');
    if (paren == std::string::npos)
        return false;
    const auto close = s.find(')', paren + 1);
    if (close == std::string::npos)
        return false;

    paramsOut = trim(s.substr(paren + 1, close - (paren + 1)));

    std::string left = trim(s.substr(0, paren));
    std::string rem;
    modifiersOut = parseLeadingModifiers(left, rem);

    if (rem.empty())
        return false;
    std::vector<std::string> parts;
    {
        size_t i = 0;
        while (i < rem.size()) {
            while (i < rem.size() && (rem[i] == ' ' || rem[i] == '\t'))
                ++i;
            if (i >= rem.size()) break;
            size_t j = i;
            while (j < rem.size() && rem[j] != ' ' && rem[j] != '\t')
                ++j;
            parts.push_back(rem.substr(i, j - i));
            i = j;
        }
    }
    if (parts.empty())
        return false;
    nameOut = parts.back();
    returnTypeOut.clear();
    if (parts.size() >= 2)
        returnTypeOut = parts[parts.size() - 2];
    return true;
}

static int countParamsTopLevel(const std::string& params) {
    size_t i = 0;
    while (i < params.size() && (params[i] == ' ' || params[i] == '\t'))
        ++i;
    if (i >= params.size())
        return 0;

    int depthAngle = 0;
    int count = 1;
    for (char c : params) {
        if (c == '<') ++depthAngle;
        else if (c == '>' && depthAngle > 0) --depthAngle;
        else if (c == ',' && depthAngle == 0) ++count;
    }
    return count;
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

    size_t lineCount = 0;

    while (std::getline(f, line)) {
        if (++lineCount % 200 == 0 && g_progressCb && totalSize > 0) {
            const auto pos = static_cast<std::uintmax_t>(f.tellg());
            int percent = static_cast<int>((pos * 100) / totalSize);
            g_progressCb(std::clamp(percent, 0, 100));
        }

        std::string s = trim(line);

        {
            std::string asmName;
            int baseIdx = -1;
            if (tryParseImageLine(s, asmName, baseIdx)) {
                ImageMapEntry e;
                e.assembly = std::move(asmName);
                e.baseTypeDefIndex = baseIdx;
                images.push_back(std::move(e));
                continue;
            }
        }

        {
            std::string ns;
            if (tryParseNamespaceLine(s, ns)) {
                currentNs = std::move(ns);
                if (currentNs.empty()) currentNs = "-";
                continue;
            }
        }

        {
            std::string kind;
            std::string typeName;
            if (tryParseTypeLine(s, kind, typeName)) {
                out.push_back({});
                currentType = &out.back();
                currentType->name = std::move(typeName);
                currentType->nameSpace = currentNs.empty() ? "-" : currentNs;
                currentType->isEnum = (kind == "enum");

                int typeDefIdx = -1;
                if (tryParseTypeDefIndex(s, typeDefIdx)) {
                    currentType->typeDefIndex = typeDefIdx;
                    std::string asmResolved;
                    int bestBase = -1;
                    for (const auto& img : images) {
                        if (img.baseTypeDefIndex <= currentType->typeDefIndex && img.baseTypeDefIndex >= bestBase) {
                            bestBase = img.baseTypeDefIndex;
                            asmResolved = img.assembly;
                        }
                    }
                    currentType->assembly = std::move(asmResolved);
                }

                section.clear();
                hasPending = false;
                continue;
            }
        }

        if (!currentType)
            continue;

        {
            std::string sec;
            if (tryParseSectionLine(s, sec)) {
                section = std::move(sec);
                continue;
            }
        }

        if (section.empty())
            continue;

        if (currentType->isEnum && section == "Fields") {
            if (s.find("value__") != std::string::npos) {
                DumpMember mem;
                mem.kind = MemberKind::Field;
                mem.signature = stripInlineComment(s);

                uint64_t off = 0;
                if (tryExtractFirstInlineHex(s, off))
                    mem.offset = off;

                currentType->members.push_back(mem);
                continue;
            }

            {
                const auto eq = s.find('=');
                if (eq != std::string::npos) {
                    const auto semi = s.find(';', eq + 1);
                    const std::string left = trim(s.substr(0, eq));
                    std::string right = (semi == std::string::npos) ? trim(s.substr(eq + 1)) : trim(s.substr(eq + 1, semi - (eq + 1)));

                    size_t j = left.size();
                    while (j > 0 && (left[j - 1] == ' ' || left[j - 1] == '\t'))
                        --j;
                    size_t i = j;
                    while (i > 0) {
                        const char c = left[i - 1];
                        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
                        if (!ok) break;
                        --i;
                    }
                    const std::string name = left.substr(i, j - i);
                    if (!name.empty()) {
                        DumpMember mem;
                        mem.kind = MemberKind::EnumValue;
                        mem.name = name;
                        mem.signature = mem.name + " = " + right;
                        currentType->members.push_back(mem);
                    }
                }
            }
            continue;
        }

        if (section == "Methods") {
            if (s.rfind("//", 0) == 0 &&
                (s.find("RVA:") != std::string::npos ||
                 s.find("Offset:") != std::string::npos ||
                 s.find("VA:") != std::string::npos)) {

                {
                    uint64_t v = 0;
                    if (tryExtractFirstHexAfter(s, "RVA:", v)) pendingRva = v;
                }
                {
                    uint64_t v = 0;
                    if (tryExtractFirstHexAfter(s, "Offset:", v)) pendingOff = v;
                }
                {
                    uint64_t v = 0;
                    if (tryExtractFirstHexAfter(s, "VA:", v)) pendingVa = v;
                }

                hasPending = true;
                continue;
            }

            if (s.empty() || s == "{" || s == "}" || s.rfind("//", 0) == 0)
                continue;

            if (!hasPending)
                continue;

            std::string modifiers;
            std::string returnType;
            std::string methodName;
            std::string params;
            if (!tryParseMethodLike(s, modifiers, returnType, methodName, params))
                continue;

            DumpMember mem;
            mem.name = methodName;
            mem.paramCount = countParamsTopLevel(params);

            if (mem.name == currentType->name) {
                mem.kind = MemberKind::Ctor;
                mem.name = ".ctor";
                returnType.clear();
            } else {
                if (mem.name.rfind("get_", 0) == 0 || mem.name.rfind("set_", 0) == 0)
                    mem.kind = MemberKind::Property;
                else if (mem.name.rfind("add_", 0) == 0 || mem.name.rfind("remove_", 0) == 0)
                    mem.kind = MemberKind::Event;
                else
                    mem.kind = MemberKind::Method;
            }

            mem.signature.clear();
            if (!modifiers.empty()) {
                mem.signature += modifiers;
                mem.signature += " ";
            }
            if (!returnType.empty() && mem.kind != MemberKind::Ctor) {
                mem.signature += returnType;
                mem.signature += " ";
            }
            mem.signature += mem.name + "(" + params + ")";

            mem.rva = pendingRva;
            mem.offset = pendingOff ? pendingOff : pendingRva;
            mem.va = pendingVa;
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

            uint64_t off = 0;
            if (tryExtractFirstInlineHex(s, off))
                mem.offset = off;

            currentType->members.push_back(mem);
            continue;
        }
    }

    if (g_progressCb)
        g_progressCb(100);

    return out;
}
