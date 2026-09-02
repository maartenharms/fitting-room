// Pure-logic tests for the Advanced-condition clause tokenizer/shape-checker.
// No engine, no RE:: types - see AdvancedConditionParse.h's banner for what
// this layer does and does not check. Advanced conditions are parameterless
// only (AdvancedCondition.cpp's file banner has the reason); whether a
// function name is real and condition-usable needs RE::SCRIPT_FUNCTION and
// lives in AdvancedCondition.cpp instead, untestable offline and therefore
// NOT covered here.
#include "AdvancedConditionParse.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

int main() {
    using namespace OS::AdvancedConditionParse;

    {  // ParseOp recognises all six operators and rejects anything else
        CHECK(ParseOp("==") == Op::kEq);
        CHECK(ParseOp("!=") == Op::kNe);
        CHECK(ParseOp(">") == Op::kGt);
        CHECK(ParseOp(">=") == Op::kGe);
        CHECK(ParseOp("<") == Op::kLt);
        CHECK(ParseOp("<=") == Op::kLe);
        CHECK(!ParseOp("="));
        CHECK(!ParseOp(""));
        CHECK(!ParseOp("eq"));
    }

    {  // Tokenize splits on whitespace, tolerates runs of it and leading/
       // trailing space, and does not choke on an empty string
        const auto t1 = Tokenize("IsSneaking == 1");
        CHECK(t1.size() == 3);
        CHECK(t1[0] == "IsSneaking");
        CHECK(t1[1] == "==");
        CHECK(t1[2] == "1");

        const auto t2 = Tokenize("  GetDistance   Skyrim.esm|0x14  <=   256  ");
        CHECK(t2.size() == 4);
        CHECK(t2[1] == "Skyrim.esm|0x14");

        CHECK(Tokenize("").empty());
        CHECK(Tokenize("   ").empty());
    }

    {  // TryParseFormParam: the documented "Plugin.esp|0xID" shape
        const auto f = TryParseFormParam("Skyrim.esm|0x14");
        CHECK(f.has_value());
        CHECK(f->modName == "Skyrim.esm");
        CHECK(f->localFormID == 0x14);

        const auto upper = TryParseFormParam("Skyrim.esm|0X1A26F");
        CHECK(upper.has_value());
        CHECK(upper->localFormID == 0x1A26F);
    }

    {  // TryParseFormParam refuses the shape rather than guess: no '|', an
       // empty mod name, an empty id, and - deliberately - a missing "0x"
       // prefix (see the header comment: decimal-vs-hex ambiguity is a
       // refusal, not a silent guess)
        CHECK(!TryParseFormParam("Skyrim.esm"));
        CHECK(!TryParseFormParam("|0x14"));
        CHECK(!TryParseFormParam("Skyrim.esm|"));
        CHECK(!TryParseFormParam("Skyrim.esm|0x"));
        CHECK(!TryParseFormParam("Skyrim.esm|20"));   // no 0x prefix - refused, not read as decimal 20
        CHECK(!TryParseFormParam("Skyrim.esm|0xZZ"));  // not valid hex
    }

    {  // TryParseIntParam: decimal, hex, negative
        CHECK(TryParseIntParam("5") == 5);
        CHECK(TryParseIntParam("0x5") == 5);
        CHECK(TryParseIntParam("0X10") == 16);
        CHECK(TryParseIntParam("-5") == -5);
        CHECK(TryParseIntParam("-0x10") == -16);
        CHECK(!TryParseIntParam(""));
        CHECK(!TryParseIntParam("-"));
        CHECK(!TryParseIntParam("abc"));
        CHECK(!TryParseIntParam("5abc"));  // trailing garbage must not be silently ignored
    }

    std::string error;

    {  // The parameterless syntax example, verbatim
        error.clear();
        const auto c1 = Parse("IsSneaking == 1", error);
        CHECK(c1.has_value());
        CHECK(c1->functionName == "IsSneaking");
        CHECK(c1->op == Op::kEq);
        CHECK(c1->value == 1.0f);
        CHECK(error.empty());
    }

    {  // Negative comparison value
        error.clear();
        const auto c = Parse("GetRelationshipRank == -2", error);
        CHECK(c.has_value());
        CHECK(c->value == -2.0f);
    }

    {  // A clause naming a parameter is refused outright, never accepted -
       // this is the review-mandated fix: a form-shaped token would
       // otherwise resolve to SOME live TESForm* regardless of whether it is
       // the FormType the real function wants, which is silent type
       // confusion, not a parse error. The refusal message names what was
       // found, which is the behaviour the review asked to keep pinned.
        error.clear();
        CHECK(!Parse("GetDistance Skyrim.esm|0x14 <= 256", error));
        CHECK(error.find("GetDistance") != std::string::npos);
        CHECK(error.find("Skyrim.esm|0x14") != std::string::npos);
        CHECK(error.find("parameter") != std::string::npos);

        // Same refusal regardless of the parameter's shape - an int-shaped
        // parameter, or a garbage token that is neither, must not be
        // accepted just because it happens not to look like a form.
        error.clear();
        CHECK(!Parse("SomeFn 5 == 1", error));
        CHECK(error.find("parameter") != std::string::npos);

        error.clear();
        CHECK(!Parse("SomeFn nonsense == 1", error));
        CHECK(error.find("parameter") != std::string::npos);

        // Two parameters (mixing a form and an int token) - still refused,
        // still names the first one.
        error.clear();
        CHECK(!Parse("SomeFn A.esp|0x10 5 == 1", error));
        CHECK(error.find("A.esp|0x10") != std::string::npos);
    }

    {  // Missing operator/value - too few tokens
        error.clear();
        CHECK(!Parse("IsSneaking", error));
        CHECK(!error.empty());

        error.clear();
        CHECK(!Parse("", error));
        CHECK(!error.empty());
    }

    {  // Unknown operator produces a readable, specific error
        error.clear();
        CHECK(!Parse("IsSneaking = 1", error));
        CHECK(error.find("=") != std::string::npos);
    }

    {  // The trailing value must be a number
        error.clear();
        CHECK(!Parse("IsSneaking == yes", error));
        CHECK(!error.empty());
    }

    {  // A token missing the "0x" prefix ("Skyrim.esm|20") does not parse as
       // a form reference (TryParseFormParam requires the prefix) or as a
       // plain integer (it still contains '.' and '|') - but the clause is
       // refused anyway, for the same "no parameters" reason as every other
       // parameter shape above, not specifically because of the missing
       // prefix.
        error.clear();
        CHECK(!Parse("GetDistance Skyrim.esm|20 <= 256", error));
        CHECK(error.find("parameter") != std::string::npos);
    }

    if (g_failures == 0) {
        std::printf("all AdvancedConditionParse tests passed\n");
    }
    return g_failures;
}
