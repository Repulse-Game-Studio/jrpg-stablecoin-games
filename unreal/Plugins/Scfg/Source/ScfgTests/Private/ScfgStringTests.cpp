// Copyright 2026 Repulse
// No Python twin: ScfgString (the glue's UTF-8 <-> FString bridge) and its lone surrogate policy (ScfgString.h).
#include "Misc/AutomationTest.h"

#include "ScfgString.h"

#include <initializer_list>
#include <string>

#if WITH_DEV_AUTOMATION_TESTS

namespace ScfgStringTests
{

// An FString of exactly these UTF-16 code units (a NUL included: AppendChar would drop it).
FString Units(std::initializer_list<UTF16CHAR> CodeUnits)
{
	TArray<TCHAR> Chars;
	for (UTF16CHAR Unit : CodeUnits)
	{
		Chars.Add(static_cast<TCHAR>(Unit));
	}
	return FString::ConstructFromPtrSize(Chars.GetData(), Chars.Num());
}

FString Hex(const std::string& Bytes)
{
	FString Out;
	for (unsigned char Byte : Bytes)
	{
		Out += FString::Printf(TEXT("%02X "), Byte);
	}
	return Out.TrimEnd();
}

FString CodeUnitsOf(const FString& S)
{
	FString Out;
	for (TCHAR C : S)
	{
		Out += FString::Printf(TEXT("%04X "), static_cast<uint32>(C));
	}
	return Out.TrimEnd();
}

}  // namespace ScfgStringTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScfgStringRoundTripTest, "Scfg.Glue.String.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Valid text crosses both ways unchanged, the astral planes included (a surrogate pair on the FString side).
bool FScfgStringRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace ScfgStringTests;
	struct FCase
	{
		const TCHAR* What;
		std::string Utf8;
		FString Text;
	};
	const FCase Cases[] = {
		{TEXT("empty"), "", FString()},
		{TEXT("ASCII"), "Chapter 2: 4.99 USDC", TEXT("Chapter 2: 4.99 USDC")},
		{TEXT("two bytes (U+00E9)"), "caf\xC3\xA9", Units({u'c', u'a', u'f', 0x00E9})},
		{TEXT("three bytes (U+20AC, U+FFFD)"), "\xE2\x82\xAC\xEF\xBF\xBD", Units({0x20AC, 0xFFFD})},
		{TEXT("emoji U+1F600 (astral)"), "\xF0\x9F\x98\x80", Units({0xD83D, 0xDE00})},
		{TEXT("CJK extension B U+20000"), "\xF0\xA0\x80\x80", Units({0xD840, 0xDC00})},
		{TEXT("the last code point U+10FFFF"), "\xF4\x8F\xBF\xBF", Units({0xDBFF, 0xDFFF})},
		{TEXT("an embedded NUL"), std::string("a\0b", 3), Units({u'a', 0, u'b'})},
		{TEXT("mixed"), "x\xF0\x9F\x92\xB0y\xC3\xA9", Units({u'x', 0xD83D, 0xDCB0, u'y', 0x00E9})},
	};
	for (const FCase& Case : Cases)
	{
		const std::string Bytes = ScfgString::ToUtf8(Case.Text);
		TestEqual(FString::Printf(TEXT("%s: ToUtf8"), Case.What), Hex(Bytes), Hex(Case.Utf8));
		const FString Back = ScfgString::FromUtf8(Case.Utf8);
		TestEqual(FString::Printf(TEXT("%s: FromUtf8"), Case.What), CodeUnitsOf(Back), CodeUnitsOf(Case.Text));
		TestEqual(FString::Printf(TEXT("%s: length"), Case.What), Back.Len(), Case.Text.Len());
		TestTrue(FString::Printf(TEXT("%s: round trip"), Case.What), ScfgString::ToUtf8(Back) == Case.Utf8);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScfgStringLoneSurrogateTest, "Scfg.Glue.String.LoneSurrogate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// The policy: an unpaired surrogate, and each invalid UTF-8 sequence, becomes one '?' (the engine's
// UNICODE_BOGUS_CHAR_CODEPOINT); the text around it is kept.
bool FScfgStringLoneSurrogateTest::RunTest(const FString& Parameters)
{
	using namespace ScfgStringTests;
	struct FToUtf8
	{
		const TCHAR* What;
		FString Text;
		std::string Utf8;
	};
	const FToUtf8 Encodes[] = {
		{TEXT("a lone high surrogate"), Units({u'a', 0xD800, u'b'}), "a?b"},
		{TEXT("a lone low surrogate"), Units({u'a', 0xDC00, u'b'}), "a?b"},
		{TEXT("a high surrogate at the end"), Units({u'a', 0xD83D}), "a?"},
		{TEXT("a pair in the wrong order"), Units({0xDE00, 0xD83D}), "??"},
		{TEXT("two high surrogates, then a pair"), Units({0xD800, 0xD83D, 0xDE00}), "?\xF0\x9F\x98\x80"},
	};
	for (const FToUtf8& Case : Encodes)
	{
		TestEqual(FString::Printf(TEXT("ToUtf8: %s"), Case.What), Hex(ScfgString::ToUtf8(Case.Text)), Hex(Case.Utf8));
	}
	struct FFromUtf8
	{
		const TCHAR* What;
		std::string Utf8;
		FString Text;
	};
	const FFromUtf8 Decodes[] = {
		// the core's WTF-8 form of a JSON "\ud800" escape (Json.h)
		{TEXT("an encoded surrogate (WTF-8 ED A0 80)"), "a\xED\xA0\x80" "b", TEXT("a?b")},
		{TEXT("an encoded pair (CESU-8)"), "\xED\xA0\xBD\xED\xB8\x80", TEXT("??")},
		{TEXT("a stray continuation byte"), "a\x80" "b", TEXT("a?b")},
		{TEXT("a byte that never starts UTF-8"), "\xFF", TEXT("?")},
		{TEXT("a sequence cut short at the end"), "a\xE2\x82", TEXT("a?")},
		{TEXT("a sequence cut short by ASCII"), "\xE2\x82z", TEXT("?z")},
		{TEXT("an overlong '/'"), "\xC0\xAF", TEXT("?")},
		{TEXT("beyond U+10FFFF"), "\xF4\x90\x80\x80", TEXT("?")},
		{TEXT("the noncharacter U+FFFF"), "\xEF\xBF\xBF", TEXT("?")},
	};
	for (const FFromUtf8& Case : Decodes)
	{
		TestEqual(FString::Printf(TEXT("FromUtf8: %s"), Case.What), CodeUnitsOf(ScfgString::FromUtf8(Case.Utf8)),
			CodeUnitsOf(Case.Text));
	}
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
