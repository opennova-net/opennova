class_name ConfigStore
extends RefCounted

## The load-modify-save helper behind the small user:// ConfigFile stores used
## by the game runtime and the NovaWorld client. Each caller owns its path,
## section, keys, and domain rules; this class owns only the disk protocol.


## The stored value, or `default` when the file or key is absent.
static func read(path: String, section: String, key: String, default: Variant) -> Variant:
	var config := ConfigFile.new()
	if config.load(path) != OK:
		return default
	return config.get_value(section, key, default)


## Load-modify-save one key, preserving everything else in the file.
static func write(path: String, section: String, key: String, value: Variant) -> void:
	var config := ConfigFile.new()
	config.load(path)
	config.set_value(section, key, value)
	config.save(path)


## Load, hand the live ConfigFile to `mutate`, save. For multi-key writes that
## must land as one disk write (e.g. a value plus its recent-list merge).
static func update(path: String, mutate: Callable) -> void:
	var config := ConfigFile.new()
	config.load(path)
	mutate.call(config)
	config.save(path)
