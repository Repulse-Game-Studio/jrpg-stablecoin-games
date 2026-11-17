// No Python twin: the details panel of the SCFG project settings, Unity's ScfgSettingsEditor inspector: a Validate
// category at the top with the button (ScfgEditor::ValidateSettings, with a notification), and Configure()'s options
// shown as rows of the Configure category rather than one folded struct.
#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UScfgSettings;

class FScfgSettingsDetails final : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
	TWeakObjectPtr<UScfgSettings> Settings;
};
