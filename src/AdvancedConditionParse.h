#pragma once

// Pure shape-checking half of the Advanced-condition parser. No RE::, no
// engine - this header compiles into a pure-logic test executable, same
// discipline as RuleModel.h and WeaponSlots.h (see
// tests/test_advancedconditionparse.cpp).
//
// Splits and validates the TEXT SHAPE of one clause:
//
//   FunctionName <op> <value>
//   IsSneaking == 1
//   IsInCombat == 0
//
// Advanced conditions are PARAMETERLESS ONLY - see AdvancedCondition.cpp's
// file banner for why (a form-shaped token resolves to SOME live TESForm*
// regardless of what FormType the target function actually wants, and
// Bethesda's condition-function bodies cast without checking, so a
// resolvable-but-wrong-kind form is silent type confusion, not a refusal).
// A clause naming any parameter at all - "GetDistance Skyrim.esm|0x14 <=
// 256" - is refused here, unconditionally, before RE:: is ever involved.
//
// What this header can answer without the engine: whether the operator is
// one of the six known symbols, whether the trailing value parses as a
// number, and - used only to make a REFUSAL message clearer, not to accept
// anything - whether a token looks like a plugin form reference
// ("Plugin.esp|0xID") or a plain integer. FormParam/IntParam/TryParseFormParam
// /TryParseIntParam are no longer wired into Parse()'s accept path, but are
// kept, tested, and commented deliberately: the rationale for going
// parameterless is that it is reversible - parameters can come back later
// behind a real per-function FormType table, and these are the building
// blocks that table would need. This header has NO opinion on whether the
// function name is real or condition-usable - that needs RE::SCRIPT_FUNCTION
// and lives in AdvancedCondition.cpp instead.

#include <charconv>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace OS::AdvancedConditionParse {

    enum class Op : std::uint8_t { kEq, kNe, kGt, kGe, kLt, kLe };

    [[nodiscard]] inline std::optional<Op> ParseOp(std::string_view a_text) {
        if (a_text == "==") return Op::kEq;
        if (a_text == "!=") return Op::kNe;
        if (a_text == ">=") return Op::kGe;
        if (a_text == "<=") return Op::kLe;
        if (a_text == ">") return Op::kGt;
        if (a_text == "<") return Op::kLt;
        return std::nullopt;
    }

    // One function-call parameter, shape-checked but not yet resolved to a
    // live TESForm* (that needs RE::TESDataHandler).
    struct FormParam {
        std::string   modName;
        std::uint32_t localFormID{ 0 };

        friend bool operator==(const FormParam&, const FormParam&) = default;
    };
    struct IntParam {
        std::int32_t value{ 0 };

        friend bool operator==(const IntParam&, const IntParam&) = default;
    };
    using Param = std::variant<FormParam, IntParam>;

    // Parameterless by construction: Parse() below refuses any clause naming
    // a parameter, so there is no params field to carry one.
    struct ParsedClause {
        std::string functionName;
        Op          op{ Op::kEq };
        float       value{ 0.0f };
    };

    // Recognises "Plugin.ext|0xID" - the '|' must be present with text on
    // both sides, and the right side MUST carry the "0x"/"0X" prefix. That
    // last part is deliberate, not an oversight: without it, "Plugin.esp|20"
    // would silently mean something different depending on whether it is
    // read as decimal 20 or (the author's likely intent, given the
    // documented syntax always shows "0x...") hex 0x20. Requiring the
    // prefix turns that ambiguity into a clean refusal - the token then
    // falls through to TryParseIntParam, which also fails on a string
    // containing '.' and '|', so the caller reports a single unambiguous
    // "not a number or a form reference" error instead of silently
    // resolving the wrong form.
    [[nodiscard]] inline std::optional<FormParam> TryParseFormParam(std::string_view a_token) {
        const auto bar = a_token.find('|');
        if (bar == std::string_view::npos || bar == 0 || bar + 1 >= a_token.size()) {
            return std::nullopt;
        }
        const std::string_view modName = a_token.substr(0, bar);
        std::string_view       idText  = a_token.substr(bar + 1);
        if (idText.size() <= 2 || idText[0] != '0' || (idText[1] != 'x' && idText[1] != 'X')) {
            return std::nullopt;
        }
        idText = idText.substr(2);

        std::uint32_t     id  = 0;
        const auto [ptr, ec] = std::from_chars(idText.data(), idText.data() + idText.size(), id, 16);
        if (ec != std::errc{} || ptr != idText.data() + idText.size()) {
            return std::nullopt;
        }
        return FormParam{ std::string(modName), id };
    }

    // Plain integer literal: optional leading '-', optional "0x"/"0X" hex.
    //
    // Cannot represent INT32_MIN: the sign is stripped and re-applied to a
    // positive magnitude, and 2147483648 (the un-negated magnitude of
    // INT32_MIN) does not fit in int32_t, so from_chars reports it out of
    // range and this returns nullopt instead of -2147483648. Fails safe (a
    // refusal, not a wrong value), and this function is not currently wired
    // into anything RE:: touches - not fixed for that reason, not because it
    // is unfixable.
    [[nodiscard]] inline std::optional<std::int32_t> TryParseIntParam(std::string_view a_token) {
        bool             negative = false;
        std::string_view text     = a_token;
        if (!text.empty() && text[0] == '-') {
            negative = true;
            text     = text.substr(1);
        }
        int base = 10;
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
            text = text.substr(2);
            base = 16;
        }
        if (text.empty()) {
            return std::nullopt;
        }
        std::int32_t magnitude    = 0;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), magnitude, base);
        if (ec != std::errc{} || ptr != text.data() + text.size()) {
            return std::nullopt;
        }
        return negative ? -magnitude : magnitude;
    }

    // Splits on whitespace. No quoting - none of the syntax's tokens
    // (function names, Plugin.esp|0xID, operators, numbers) ever contain a
    // space, so none is needed.
    [[nodiscard]] inline std::vector<std::string> Tokenize(const std::string& a_text) {
        std::vector<std::string> out;
        std::istringstream       stream{ a_text };
        std::string              tok;
        while (stream >> tok) {
            out.push_back(tok);
        }
        return out;
    }

    // Splits and shape-checks a_text into a ParsedClause. Returns nullopt and
    // fills a_error with a user-readable reason on any failure.
    [[nodiscard]] inline std::optional<ParsedClause> Parse(const std::string& a_text,
                                                            std::string&       a_error) {
        const auto tokens = Tokenize(a_text);
        if (tokens.size() < 3) {
            a_error = "Expected 'FunctionName <op> <value>', got: '" + a_text + "'";
            return std::nullopt;
        }

        ParsedClause clause;
        clause.functionName = tokens.front();

        const auto op = ParseOp(tokens[tokens.size() - 2]);
        if (!op) {
            a_error = "Unknown operator '" + tokens[tokens.size() - 2] + "'";
            return std::nullopt;
        }
        clause.op = *op;

        const std::string& valueText = tokens.back();
        float               value    = 0.0f;
        const auto [vptr, vec] =
            std::from_chars(valueText.data(), valueText.data() + valueText.size(), value);
        if (vec != std::errc{} || vptr != valueText.data() + valueText.size()) {
            a_error = "'" + valueText + "' is not a number";
            return std::nullopt;
        }
        clause.value = value;

        // Parameterless only (see the file banner): anything between the
        // function name and the operator means the clause names a
        // parameter, which is refused unconditionally here rather than
        // parsed - regardless of whether it looks like a form reference, an
        // integer, or neither. TryParseFormParam is still used, but only to
        // make the refusal message name what it saw.
        if (tokens.size() != 3) {
            const std::string& firstParamTok = tokens[1];
            if (TryParseFormParam(firstParamTok)) {
                a_error = "'" + clause.functionName + "' was given a parameter ('" + firstParamTok +
                          "'). Only parameterless conditions (e.g. 'IsSneaking == 1') can be used "
                          "here.";
            } else {
                a_error = "'" + clause.functionName +
                          "' was given a parameter. Only parameterless conditions (e.g. "
                          "'IsSneaking == 1') can be used here.";
            }
            return std::nullopt;
        }

        return clause;
    }

}  // namespace OS::AdvancedConditionParse
