// No Python twin: see ScfgSettingsDetails.h.
#include "ScfgSettingsDetails.h"

#include "ScfgSettings.h"
#include "ScfgSettingsValidation.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "PropertyHandle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ScfgSettingsDetails"

TSharedRef<IDetailCustomization> FScfgSettingsDetails::MakeInstance()
{
	return MakeShared<FScfgSettingsDetails>();
}

void FScfgSettingsDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	Settings = Objects.Num() == 1 ? Cast<UScfgSettings>(Objects[0].Get()) : nullptr;

	IDetailCategoryBuilder& Validate =
		DetailBuilder.EditCategory(TEXT("Validate"), LOCTEXT("ValidateCategory", "Validate"), ECategoryPriority::Important);
	TWeakObjectPtr<UScfgSettings> Weak = Settings;
	Validate.AddCustomRow(LOCTEXT("ValidateFilter", "Validate"))
		.WholeRowContent()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 4.f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(LOCTEXT("Help",
					"Applied when the game instance starts (only with a wallet set), through AddChain, Configure and "
					"AddItem. A Configure() call from your code replaces these settings."))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Left)
			.Padding(0.f, 4.f)
			[
				SNew(SButton)
				.Text(LOCTEXT("ValidateButton", "Validate"))
				.ToolTipText(LOCTEXT("ValidateTip",
					"Run the checks SCFG runs at startup on these settings; the result is shown on the SCFG page of "
					"the Message Log."))
				.IsEnabled_Lambda([Weak]() { return Weak.IsValid(); })
				.OnClicked_Lambda([Weak]()
				{
					if (const UScfgSettings* S = Weak.Get())
					{
						ScfgEditor::ValidateSettings(*S, /*bNotify=*/true);
					}
					return FReply::Handled();
				})
			]
		];

	// Configure()'s options as rows of their own category (the struct's fields, not one folded "Options" row).
	const TSharedRef<IPropertyHandle> Options = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UScfgSettings, Options));
	uint32 NumChildren = 0;
	if (Options->IsValidHandle() && Options->GetNumChildren(NumChildren) == FPropertyAccess::Success && NumChildren > 0)
	{
		IDetailCategoryBuilder& Configure = DetailBuilder.EditCategory(TEXT("Configure"));
		DetailBuilder.HideProperty(Options);
		for (uint32 I = 0; I < NumChildren; ++I)
		{
			Configure.AddProperty(Options->GetChildHandle(I));
		}
	}
}

#undef LOCTEXT_NAMESPACE
