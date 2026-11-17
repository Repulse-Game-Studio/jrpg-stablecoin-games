// No Python twin: the conformance vectors as Unreal automation tests, run inside the editor process (UBT's clang and
// bundled libc++, the engine's allocator, its locale and its stack) by the same runners as the CMake binary:
//
//   Scfg.Core.Tier1.<topic>              core-tests/support/Tier1 over conformance/vectors/tier1/<topic>.json
//   Scfg.Core.Tier2.<group>.<name>       core-tests/support/Tier2 over tier2/<group>/<name>.json, immediate transport
//   Scfg.Core.Tier2Deferred.<group>.<name>  the same, requests answered one per pump turn (Unreal's HTTP callbacks)
//   Scfg.Core.NothingLeftOut             the three above have one test per vector file: 18 topics, 121 vectors
//
// A vector id's "/" is a "." in the test name, so the session frontend shows the vector groups as folders. The
// vectors are $SCFG_VECTORS, else the conformance/vectors found by walking up from the Scfg plugin's folder.
// Run them with tools/run_unreal_tests.py (unreal/README.md, "Under UBT").
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#include "support/Tier1.h"
#include "support/Tier2.h"
#include "support/Vectors.h"

#include "Scfg/Json.h"
#include "Scfg/Result.h"

#include <algorithm>
#include <clocale>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if WITH_DEV_AUTOMATION_TESTS

namespace ScfgCoreAutomation
{

// The vector files of this checkout; a vector added or removed changes these on purpose (and run_unreal_tests.py's
// --expect in the docs).
constexpr int32 kTier1Topics = 18;
constexpr int32 kTier2Vectors = 121;
// The command of the one test a listing that fails or finds nothing becomes (no vector file is named so).
const FString kListing = TEXT("!listing");
// At most this many failures of one test are reported one by one.
constexpr size_t kShown = 20;

FString Text(std::string_view S)
{
	const auto Converted = StringCast<TCHAR>(reinterpret_cast<const UTF8CHAR*>(S.data()), static_cast<int32>(S.size()));
	return FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());
}

std::string Utf8(const FString& S)
{
	const auto Converted = StringCast<UTF8CHAR>(*S, S.Len());
	return std::string(reinterpret_cast<const char*>(Converted.Get()), static_cast<size_t>(Converted.Length()));
}

// vectors::root() reads $SCFG_VECTORS itself; without it, the vectors above the plugin's folder (the CMake build
// compiles the repository's path in instead, which an Unreal module has no business knowing).
void UseTheVectorsAboveThePlugin()
{
	static bool bDone = false;
	if (bDone)
	{
		return;
	}
	bDone = true;
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Scfg"));
	if (!Plugin.IsValid())
	{
		return;  // root() stays "", and the listing test says where it looked
	}
	const FString Base = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
	if (const std::optional<std::string> Found = scfg_test::vectors::find_above(Utf8(Base)))
	{
		scfg_test::vectors::set_fallback(*Found);
	}
}

// One test per name, a vector id's "/" shown as ".", or the listing test when there is nothing to list.
void AddTests(const std::vector<std::string>& Names, const std::string& Error, TArray<FString>& OutBeautifiedNames,
	TArray<FString>& OutTestCommands)
{
	if (!Error.empty() || Names.empty())
	{
		OutBeautifiedNames.Add(TEXT("Listing"));
		OutTestCommands.Add(kListing);
	}
	for (const std::string& Name : Names)
	{
		OutBeautifiedNames.Add(Text(Name).Replace(TEXT("/"), TEXT(".")));
		OutTestCommands.Add(Text(Name));
	}
}

void ReportListing(FAutomationTestBase& Test, std::string_view Dir, const std::string& Error)
{
	Test.AddError(Text("cannot list " + scfg_test::vectors::root() + "/" + std::string(Dir) + ": " +
		(Error.empty() ? std::string("no vectors") : Error)));
}

void ReportFailures(FAutomationTestBase& Test, const std::vector<std::string>& Failures)
{
	for (size_t N = 0; N < Failures.size() && N < kShown; ++N)
	{
		Test.AddError(Text(Failures[N]));
	}
	if (Failures.size() > kShown)
	{
		Test.AddError(FString::Printf(TEXT("... and %d more"), static_cast<int32>(Failures.size() - kShown)));
	}
}

void ListTier2(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands)
{
	UseTheVectorsAboveThePlugin();
	std::string Error;
	const std::vector<std::string> Ids = scfg_test::vectors::tier2_ids(&Error);
	AddTests(Ids, Error, OutBeautifiedNames, OutTestCommands);
}

// Tier2Tests.cpp's and Tier2DeferredTests.cpp's bodies.
bool RunTier2(FAutomationTestBase& Test, const FString& Parameters, scfg_test::tier2::ReplayMode Mode)
{
	UseTheVectorsAboveThePlugin();
	if (Parameters == kListing)
	{
		std::string Error;
		scfg_test::vectors::tier2_ids(&Error);
		ReportListing(Test, "tier2", Error);
		return false;
	}
	const std::string Id = Utf8(Parameters);
	const std::optional<std::string> Source = scfg_test::vectors::read_text("tier2/" + Id + ".json");
	if (!Source)
	{
		Test.AddError(Text("tier2/" + Id + " cannot be read under " + scfg_test::vectors::root()));
		return false;
	}
	const scfg::Result<scfg::Json> Vector = scfg_test::tier2::parse(*Source);
	if (!Vector.ok())
	{
		Test.AddError(Text("tier2/" + Id + " cannot be parsed: " + Vector.err().str()));
		return false;
	}
	scfg_test::tier2::ReplayTransport Transport(Mode);
	const std::optional<std::string> Diff = scfg_test::tier2::replay(Vector.value(), Transport);
	const bool bDeferred = Mode == scfg_test::tier2::ReplayMode::Deferred;
	if (Diff)
	{
		Test.AddError(Text("tier2/" + Id + (bDeferred ? " (deferred): " : ": ") + *Diff));
	}
	if (Transport.queued() != 0)
	{
		Test.AddError(Text("tier2/" + Id + ": requests left in the queue"));
	}
	return !Diff && Transport.queued() == 0;
}

// The tests GetTests gives the framework under one of the complex tests: their full names, "Scfg.Core.Tier1.amounts".
TArray<FString> NamesOf(const FAutomationTestBase& Test)
{
	TArray<FAutomationTestInfo> Infos;
	Test.GenerateTestNames(Infos);
	TArray<FString> Names;
	for (const FAutomationTestInfo& Info : Infos)
	{
		Names.Add(Info.GetFullTestPath());
	}
	return Names;
}

}  // namespace ScfgCoreAutomation

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FScfgCoreTier1Test, "Scfg.Core.Tier1",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FScfgCoreTier1Test::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	ScfgCoreAutomation::UseTheVectorsAboveThePlugin();
	std::string Error;
	const std::vector<std::string> Topics = scfg_test::vectors::tier1_topics(&Error);
	ScfgCoreAutomation::AddTests(Topics, Error, OutBeautifiedNames, OutTestCommands);
}

// Tier1Tests.cpp's body: every case of the topic through the adapters its tier1/<Topic>.cpp registered.
bool FScfgCoreTier1Test::RunTest(const FString& Parameters)
{
	ScfgCoreAutomation::UseTheVectorsAboveThePlugin();
	if (Parameters == ScfgCoreAutomation::kListing)
	{
		std::string Error;
		scfg_test::vectors::tier1_topics(&Error);
		ScfgCoreAutomation::ReportListing(*this, "tier1", Error);
		return false;
	}
	const std::string Topic = ScfgCoreAutomation::Utf8(Parameters);
	const std::vector<std::string> Failures = scfg_test::tier1::run_topic(Topic);
	if (!Failures.empty())
	{
		AddError(FString::Printf(TEXT("tier1/%s: %d failing case(s)"), *Parameters, static_cast<int32>(Failures.size())));
	}
	ScfgCoreAutomation::ReportFailures(*this, Failures);
	return Failures.empty();
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FScfgCoreTier2Test, "Scfg.Core.Tier2",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FScfgCoreTier2Test::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	ScfgCoreAutomation::ListTier2(OutBeautifiedNames, OutTestCommands);
}

bool FScfgCoreTier2Test::RunTest(const FString& Parameters)
{
	return ScfgCoreAutomation::RunTier2(*this, Parameters, scfg_test::tier2::ReplayMode::Immediate);
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FScfgCoreTier2DeferredTest, "Scfg.Core.Tier2Deferred",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FScfgCoreTier2DeferredTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	ScfgCoreAutomation::ListTier2(OutBeautifiedNames, OutTestCommands);
}

bool FScfgCoreTier2DeferredTest::RunTest(const FString& Parameters)
{
	return ScfgCoreAutomation::RunTier2(*this, Parameters, scfg_test::tier2::ReplayMode::Deferred);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScfgCoreNothingLeftOutTest, "Scfg.Core.NothingLeftOut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// The listings above use std::filesystem (support/Vectors); this counts the files again through the engine's own
// file manager, and checks that the framework got one test per file from each complex test, and that every Tier 1
// topic file has its adapters (Tier1Tests.EveryTopicIsRegisteredOnceAndHasVectors).
bool FScfgCoreNothingLeftOutTest::RunTest(const FString& Parameters)
{
	using namespace ScfgCoreAutomation;
	UseTheVectorsAboveThePlugin();
	const FString Root = Text(scfg_test::vectors::root());
	AddInfo(FString::Printf(TEXT("vectors: %s"), *Root));
	const char* Locale = std::setlocale(LC_ALL, nullptr);
	AddInfo(FString::Printf(TEXT("setlocale(LC_ALL, nullptr): %s"), Locale ? *Text(Locale) : TEXT("(null)")));

	TArray<FString> Tier1Files;
	IFileManager::Get().FindFiles(Tier1Files, *(Root / TEXT("tier1")), TEXT("*.json"));
	TArray<FString> Tier2Files;
	IFileManager::Get().FindFilesRecursive(Tier2Files, *(Root / TEXT("tier2")), TEXT("*.json"), true, false);
	TestEqual(TEXT("tier1/*.json files"), Tier1Files.Num(), kTier1Topics);
	TestEqual(TEXT("tier2/**/*.json files"), Tier2Files.Num(), kTier2Vectors);

	struct FExpected
	{
		const FAutomationTestBase& Test;
		const TCHAR* Prefix;
		int32 Count;
	};
	const FExpected Expected[] = {
		{FScfgCoreTier1TestAutomationTestInstance, TEXT("Scfg.Core.Tier1."), Tier1Files.Num()},
		{FScfgCoreTier2TestAutomationTestInstance, TEXT("Scfg.Core.Tier2."), Tier2Files.Num()},
		{FScfgCoreTier2DeferredTestAutomationTestInstance, TEXT("Scfg.Core.Tier2Deferred."), Tier2Files.Num()},
	};
	for (const FExpected& E : Expected)
	{
		const TArray<FString> Names = NamesOf(E.Test);
		int32 Matching = 0;
		for (const FString& Name : Names)
		{
			if (Name.StartsWith(E.Prefix, ESearchCase::CaseSensitive) && !Name.EndsWith(TEXT(".Listing")))
			{
				++Matching;
			}
		}
		TestEqual(*FString::Printf(TEXT("%stests"), E.Prefix), Names.Num(), E.Count);
		TestEqual(*FString::Printf(TEXT("%s<vector> tests"), E.Prefix), Matching, E.Count);
	}

	const scfg_test::tier1::Registry& Registry = scfg_test::tier1::registry();
	for (const std::string& Error : Registry.errors)
	{
		AddError(Text(Error));
	}
	TestEqual(TEXT("registered Tier 1 topics"), static_cast<int32>(Registry.topics.size()), kTier1Topics);
	for (const FString& File : Tier1Files)
	{
		const std::string Topic = Utf8(FPaths::GetBaseFilename(File));
		TestTrue(*Text("tier1/" + Topic + ".json has its SCFG_TIER1_TOPIC(" + Topic + ")"),
			std::find(Registry.topics.begin(), Registry.topics.end(), Topic) != Registry.topics.end());
	}
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
