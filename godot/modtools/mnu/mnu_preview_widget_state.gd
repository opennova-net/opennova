class_name MnuPreviewWidgetState
extends RefCounted
## Observable state for one widget in the live menu preview. The canvas owns
## Control lookup and returns this typed record to editor tools and tests
## (ADR 0017); it degrades to a Dictionary only at the MCP transport boundary.

var exists := false
var visible := false
var pressable := false
var disabled := false
var pressed := false
var activated := false
