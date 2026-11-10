## The editor plugin: enabling "Stablecoin for Games (SCFG)" in Project Settings > Plugins adds
## the `Scfg` autoload (scfg.gd, next to this file), disabling it removes the autoload. This is
## the addon's only editor-only file; the game itself never loads it.
@tool
extends EditorPlugin

const AUTOLOAD_NAME := "Scfg"


func _enable_plugin() -> void:
	add_autoload_singleton(AUTOLOAD_NAME, _autoload_path())


func _disable_plugin() -> void:
	remove_autoload_singleton(AUTOLOAD_NAME)


func _autoload_path() -> String:
	return (get_script() as Script).resource_path.get_base_dir().path_join("scfg.gd")
