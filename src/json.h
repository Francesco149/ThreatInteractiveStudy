// Minimal JSON reader (enough for glTF "extras" and our small config files).
#pragma once
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <utility>

struct Json {
    enum Type { NUL, BOOL, NUMBER, STRING, ARRAY, OBJECT } type = NUL;
    double number = 0;
    bool boolean = false;
    std::string str;
    std::vector<Json> items;                         // ARRAY
    std::vector<std::pair<std::string, Json>> fields; // OBJECT

    const Json* get(const char* key) const {
        for (auto& f : fields)
            if (f.first == key) return &f.second;
        return nullptr;
    }
    double num(const char* key, double def = 0) const {
        const Json* j = get(key);
        return j && j->type == NUMBER ? j->number : (j && j->type == BOOL ? (j->boolean ? 1 : 0) : def);
    }
    std::string string(const char* key, const char* def = "") const {
        const Json* j = get(key);
        return j && j->type == STRING ? j->str : def;
    }
};

struct JsonParser {
    const char* p;
    const char* end;

    void ws() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }

    bool parse(Json& out) {
        ws();
        if (p >= end) return false;
        char c = *p;
        if (c == '{') {
            out.type = Json::OBJECT;
            p++;
            ws();
            if (*p == '}') { p++; return true; }
            while (p < end) {
                Json key;
                ws();
                if (!parse(key) || key.type != Json::STRING) return false;
                ws();
                if (*p++ != ':') return false;
                Json val;
                if (!parse(val)) return false;
                out.fields.push_back({key.str, val});
                ws();
                if (*p == ',') { p++; continue; }
                if (*p == '}') { p++; return true; }
                return false;
            }
            return false;
        }
        if (c == '[') {
            out.type = Json::ARRAY;
            p++;
            ws();
            if (*p == ']') { p++; return true; }
            while (p < end) {
                Json val;
                if (!parse(val)) return false;
                out.items.push_back(val);
                ws();
                if (*p == ',') { p++; continue; }
                if (*p == ']') { p++; return true; }
                return false;
            }
            return false;
        }
        if (c == '"') {
            out.type = Json::STRING;
            p++;
            while (p < end && *p != '"') {
                if (*p == '\\' && p + 1 < end) {
                    p++;
                    char e = *p;
                    out.str += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                } else {
                    out.str += *p;
                }
                p++;
            }
            p++;
            return true;
        }
        if (!strncmp(p, "true", 4)) { out.type = Json::BOOL; out.boolean = true; p += 4; return true; }
        if (!strncmp(p, "false", 5)) { out.type = Json::BOOL; p += 5; return true; }
        if (!strncmp(p, "null", 4)) { p += 4; return true; }
        char* num_end = nullptr;
        out.number = strtod(p, &num_end);
        if (num_end == p) return false;
        out.type = Json::NUMBER;
        p = num_end;
        return true;
    }
};

inline bool json_parse(const char* text, size_t len, Json& out) {
    JsonParser parser{text, text + len};
    return parser.parse(out);
}
