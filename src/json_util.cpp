// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#include "json_util.h"

#include "text_util.h"

#include <cctype>
#include <vector>

namespace nshmqtt
{

namespace
{

size_t skip_ws(const std::string &s, size_t i)
{
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
    {
        ++i;
    }
    return i;
}

// s[i] must be the opening '"'. Returns the index just past the closing
// (unescaped) '"', or npos if the string is never terminated.
size_t skip_json_string(const std::string &s, size_t i)
{
    ++i;
    while (i < s.size())
    {
        if (s[i] == '\\')
        {
            i += 2;
            continue;
        }
        if (s[i] == '"')
        {
            return i + 1;
        }
        ++i;
    }
    return std::string::npos;
}

// Skips one JSON value starting at s[i] (leading whitespace already
// consumed by the caller). Handles strings, objects, and arrays generically
// enough to find their true boundaries (including nested braces/brackets
// inside strings, which are not structural) without interpreting their
// content; numbers/true/false/null are consumed up to the next structural
// character. Returns the index just past the value, or npos on malformed
// input (unterminated string, unbalanced braces/brackets, empty token).
size_t skip_json_value(const std::string &s, size_t i)
{
    if (i >= s.size())
    {
        return std::string::npos;
    }
    char c = s[i];
    if (c == '"')
    {
        return skip_json_string(s, i);
    }
    if (c == '{' || c == '[')
    {
        char open = c;
        char close = (c == '{') ? '}' : ']';
        int depth = 1;
        ++i;
        while (i < s.size() && depth > 0)
        {
            if (s[i] == '"')
            {
                i = skip_json_string(s, i);
                if (i == std::string::npos)
                {
                    return std::string::npos;
                }
                continue;
            }
            if (s[i] == open)
            {
                ++depth;
            }
            else if (s[i] == close)
            {
                --depth;
            }
            ++i;
        }
        return (depth == 0) ? i : std::string::npos;
    }

    size_t start = i;
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && !std::isspace(static_cast<unsigned char>(s[i])))
    {
        ++i;
    }
    return (i > start) ? i : std::string::npos;
}

// Reverses json_escape() (text_util.cpp): \" \\ \/ \b \f \n \r \t and
// \uXXXX (BMP code points only, no surrogate-pair handling -- topic names
// are not expected to need them). An incomplete/invalid \u escape is
// passed through as literal "u" rather than failing the whole parse.
std::string json_unescape(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        char c = s[i];
        if (c != '\\')
        {
            out += c;
            continue;
        }
        ++i;
        if (i >= s.size())
        {
            break;
        }
        switch (s[i])
        {
        case '"':
            out += '"';
            break;
        case '\\':
            out += '\\';
            break;
        case '/':
            out += '/';
            break;
        case 'b':
            out += '\b';
            break;
        case 'f':
            out += '\f';
            break;
        case 'n':
            out += '\n';
            break;
        case 'r':
            out += '\r';
            break;
        case 't':
            out += '\t';
            break;
        case 'u': {
            if (i + 4 >= s.size())
            {
                out += 'u';
                break;
            }
            unsigned int cp = 0;
            bool ok = true;
            for (int k = 1; k <= 4 && ok; ++k)
            {
                char h = s[i + k];
                cp <<= 4;
                if (h >= '0' && h <= '9')
                {
                    cp |= static_cast<unsigned int>(h - '0');
                }
                else if (h >= 'a' && h <= 'f')
                {
                    cp |= static_cast<unsigned int>(h - 'a' + 10);
                }
                else if (h >= 'A' && h <= 'F')
                {
                    cp |= static_cast<unsigned int>(h - 'A' + 10);
                }
                else
                {
                    ok = false;
                }
            }
            if (!ok)
            {
                out += 'u';
                break;
            }
            if (cp < 0x80)
            {
                out += static_cast<char>(cp);
            }
            else if (cp < 0x800)
            {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            else
            {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            i += 4;
            break;
        }
        default:
            out += s[i];
        }
    }
    return out;
}

// Splits the flat top-level object starting at the first non-whitespace
// character of `text` into (unescaped key, raw still-encoded value text)
// pairs. Returns false if `text` isn't a well-formed flat object (missing
// braces, an unterminated string, unbalanced nested structure) -- callers
// that only need one field still require the whole object to parse, which
// keeps this one honest parser rather than two subtly different ones.
bool tokenize_flat_object(const std::string &text, std::vector<std::pair<std::string, std::string>> &pairs)
{
    pairs.clear();
    size_t i = skip_ws(text, 0);
    if (i >= text.size() || text[i] != '{')
    {
        return false;
    }
    ++i;
    i = skip_ws(text, i);
    if (i < text.size() && text[i] == '}')
    {
        return true; // empty object
    }

    for (;;)
    {
        i = skip_ws(text, i);
        if (i >= text.size() || text[i] != '"')
        {
            return false;
        }
        size_t key_start = i + 1;
        size_t after_key = skip_json_string(text, i);
        if (after_key == std::string::npos)
        {
            return false;
        }
        std::string raw_key = text.substr(key_start, after_key - key_start - 1);

        i = skip_ws(text, after_key);
        if (i >= text.size() || text[i] != ':')
        {
            return false;
        }
        ++i;
        i = skip_ws(text, i);

        size_t vstart = i;
        size_t vend = skip_json_value(text, i);
        if (vend == std::string::npos)
        {
            return false;
        }
        pairs.emplace_back(json_unescape(raw_key), text.substr(vstart, vend - vstart));

        i = skip_ws(text, vend);
        if (i < text.size() && text[i] == ',')
        {
            ++i;
            continue;
        }
        if (i < text.size() && text[i] == '}')
        {
            return true;
        }
        return false;
    }
}

// Recursion for flatten_json_object() below. `prefix` is the already-joined
// parent path ("" at the top level); each key found at this level is
// joined to it with '_' before being classified and either recursed into
// (object), skipped (string/null/array), or added to `out` (number/bool).
bool flatten_json_object_impl(const std::string &text, const std::string &prefix,
                              std::vector<std::pair<std::string, double>> &out)
{
    std::vector<std::pair<std::string, std::string>> pairs;
    if (!tokenize_flat_object(text, pairs))
    {
        return false;
    }
    for (const auto &kv : pairs)
    {
        std::string name = prefix.empty() ? kv.first : (prefix + "_" + kv.first);
        const std::string &raw_value = kv.second;
        if (raw_value.empty())
        {
            return false; // tokenize_flat_object never actually produces this; defensive only
        }

        char c = raw_value[0];
        if (c == '{')
        {
            if (!flatten_json_object_impl(raw_value, name, out))
            {
                return false;
            }
        }
        else if (c == '[' || c == '"')
        {
            // array or string -- ignored entirely, not a parse error
        }
        else if (raw_value == "true")
        {
            out.emplace_back(std::move(name), 1.0);
        }
        else if (raw_value == "false")
        {
            out.emplace_back(std::move(name), 0.0);
        }
        else if (raw_value == "null")
        {
            // ignored entirely, not a parse error
        }
        else
        {
            double v = 0.0;
            if (!parse_double(raw_value, v))
            {
                return false; // not a recognized JSON literal shape at all -- malformed
            }
            out.emplace_back(std::move(name), v);
        }
    }
    return true;
}

} // namespace

bool extract_json_number_field(const std::string &text, const std::string &field, double &out)
{
    std::vector<std::pair<std::string, std::string>> pairs;
    if (!tokenize_flat_object(text, pairs))
    {
        return false;
    }
    for (const auto &kv : pairs)
    {
        if (kv.first == field)
        {
            return parse_double(kv.second, out);
        }
    }
    return false;
}

bool parse_flat_number_object(const std::string &text, std::map<std::string, double> &out)
{
    out.clear();
    std::vector<std::pair<std::string, std::string>> pairs;
    if (!tokenize_flat_object(text, pairs))
    {
        return false;
    }
    for (const auto &kv : pairs)
    {
        double v = 0.0;
        if (!parse_double(kv.second, v))
        {
            return false;
        }
        out[kv.first] = v;
    }
    return true;
}

bool flatten_json_object(const std::string &text, std::vector<std::pair<std::string, double>> &out)
{
    return flatten_json_object_impl(text, "", out);
}

} // namespace nshmqtt
