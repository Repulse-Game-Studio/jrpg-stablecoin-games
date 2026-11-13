## A QR code drawn from the core's module matrix (ScfgPaylinks.qr_rects, the same geometry as
## Ren'Py's scfg.QR): black modules on white, with a quiet zone of BORDER modules around them, at
## the largest integer scale that fits `target_size` pixels (at least 1), so every module is a
## whole number of pixels and stays sharp.
##
##     var qr := ScfgQrView.new()
##     var made: Variant = qr.show_text(uri, 176)   # null, or the ScfgErr of a text too long
##
## `render_image()` paints the same rectangles into an Image (what the screen draws, for tests and
## for a game that wants the code as a texture).
class_name ScfgQrView
extends Control

const BORDER := 2

## The size the code should fit in, in pixels.
var target_size := 176
var _modules := 0  # modules per side, quiet zone included
var _runs: Array = []  # [x, y, width] dark runs, in modules, quiet zone included


func _init() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE


## Show the QR code of `text`, fitted into `target` pixels. Returns null, or the ScfgErr of a text
## no QR version can hold (nothing is shown then).
## @fallible
func show_text(text: String, target: int) -> Variant:
	target_size = target
	var made: Variant = ScfgPaylinks.qr_rects(text, BORDER)
	if made is ScfgErr:
		_modules = 0
		_runs = []
		custom_minimum_size = Vector2.ZERO
		queue_redraw()
		return made
	_modules = made[0]
	_runs = made[1]
	var side := _modules * module_scale()
	custom_minimum_size = Vector2(side, side)
	queue_redraw()
	return null


## Pixels per module: the largest integer that fits target_size, at least 1.
func module_scale() -> int:
	return maxi(1, target_size / _modules) if _modules > 0 else 1


## The dark modules as pixel rectangles.
func dark_rects() -> Array[Rect2i]:
	var s := module_scale()
	var out: Array[Rect2i] = []
	for run: Array in _runs:
		out.append(Rect2i(int(run[0]) * s, int(run[1]) * s, int(run[2]) * s, s))
	return out


## The code as an RGB Image, exactly what _draw() paints.
func render_image() -> Image:
	var side := maxi(1, _modules * module_scale())
	var img := Image.create(side, side, false, Image.FORMAT_RGB8)
	img.fill(Color.WHITE)
	for r: Rect2i in dark_rects():
		img.fill_rect(r, Color.BLACK)
	return img


func _draw() -> void:
	if _modules == 0:
		return
	var side := _modules * module_scale()
	draw_rect(Rect2(0, 0, side, side), Color.WHITE)
	for r: Rect2i in dark_rects():
		draw_rect(Rect2(r), Color.BLACK)
