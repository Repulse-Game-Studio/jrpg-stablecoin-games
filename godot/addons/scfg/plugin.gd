## The editor plugin: enabling "Stablecoin for Games (SCFG)" in Project Settings > Plugins adds
## the `Scfg` autoload (scfg.gd, next to this file), disabling it removes the autoload. While the
## plugin is on, Project Settings also shows the addon's setting `scfg/web/storage_id` (see
## web_mirror.gd; the game reads it with or without this plugin). This is the addon's only
## editor-only file; the game itself never loads it.
@tool
extends EditorPlugin

const AUTOLOAD_NAME := "Scfg"
## ScfgWebMirror.STORAGE_ID_SETTING (the test checks they agree).
const STORAGE_ID_SETTING := "scfg/web/storage_id"


func _enter_tree() -> void:
	register_settings()


## Shows `scfg/web/storage_id` in Project Settings: a String, empty by default (an empty value is
## not written to project.godot), with a placeholder. A value the project has is kept.
static func register_settings() -> void:
	if not ProjectSettings.has_setting(STORAGE_ID_SETTING):
		ProjectSettings.set_setting(STORAGE_ID_SETTING, "")
	ProjectSettings.set_initial_value(STORAGE_ID_SETTING, "")
	ProjectSettings.add_property_info({"name": STORAGE_ID_SETTING, "type": TYPE_STRING,
		"hint": PROPERTY_HINT_PLACEHOLDER_TEXT,
		"hint_string": "Web: a unique id such as com.example.mygame (empty: the project name)"})
	ProjectSettings.set_as_basic(STORAGE_ID_SETTING, true)


func _enable_plugin() -> void:
	enable(self, autoload_path(get_script()))


func _disable_plugin() -> void:
	disable(self)


## Adds the `Scfg` autoload (`path`: scfg.gd, beside this file) through `plugin`: this
## EditorPlugin (a stand-in with the same method in the tests, where no editor runs).
static func enable(plugin: Object, path: String) -> void:
	plugin.call("add_autoload_singleton", AUTOLOAD_NAME, path)


## Removes the `Scfg` autoload through `plugin` (see enable()).
static func disable(plugin: Object) -> void:
	plugin.call("remove_autoload_singleton", AUTOLOAD_NAME)


## scfg.gd beside `plugin_script` (this file), wherever the addon folder is.
static func autoload_path(plugin_script: Script) -> String:
	return plugin_script.resource_path.get_base_dir().path_join("scfg.gd")
