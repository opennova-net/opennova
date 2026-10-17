"""Pins scripts/lint/ratchet_counts.py's godot_src_dictionary_params gauge: it counts the
Dictionary parameters of the methods godot/src binds, whatever their names.

Run: python -B -m unittest discover -s scripts/lint -p "test_*.py" (CI's gates job).
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ratchet_counts  # noqa: E402

BINDINGS = """
void Browser::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_apply_ping_results", "results", "generation"),
			&Browser::apply_ping_results);
	ClassDB::bind_method(D_METHOD("sort_rows", "pings"), &Browser::sort_rows);
	ClassDB::bind_static_method("Browser", D_METHOD("from_profile", "profile"), &Browser::from_profile);
	ClassDB::bind_method(D_METHOD("open", "options"), &Other::open);
}
"""

HEADER = """
class Browser : public RefCounted {
	GDCLASS(Browser, RefCounted)
public:
	void apply_ping_results(const Dictionary &results, int64_t generation);
	void sort_rows(const TypedArray<Dictionary> &p_pings,
			const Vector3 &p_origin = Vector3(0, 0, 0));
	static Ref<Browser> from_profile(const Dictionary &p_profile = Dictionary());
	void set_cache(const Dictionary &p_cache);
	void open(const Dictionary &p_options);
	void apply() { apply_ping_results(cached_, 0); }
};
class Other : public RefCounted {
	GDCLASS(Other, RefCounted)
public:
	void open(const Dictionary &p_options, Dictionary &r_out);
};
"""


def params(header: str = HEADER, sources: tuple[str, ...] = (BINDINGS, HEADER)) -> list[tuple[str, str]]:
    bound = ratchet_counts.godot_bound_methods([ratchet_counts.CPP_COMMENT.sub("", s) for s in sources])
    return ratchet_counts.godot_bound_dictionary_params(ratchet_counts.CPP_COMMENT.sub("", header), bound)


class BoundDictionaryParams(unittest.TestCase):
    def test_the_bound_methods(self):
        bound = ratchet_counts.godot_bound_methods([BINDINGS])
        self.assertEqual(bound, {"Browser": {"apply_ping_results", "sort_rows", "from_profile"},
                                 "Other": {"open"}})

    def test_counts_bound_parameters_whatever_their_names(self):
        self.assertEqual(params(), [
            ("Browser::apply_ping_results", "const Dictionary &results"),
            ("Browser::sort_rows", "const TypedArray<Dictionary> &p_pings"),
            ("Browser::from_profile", "const Dictionary &p_profile = Dictionary()"),
            ("Other::open", "const Dictionary &p_options"),
            ("Other::open", "Dictionary &r_out"),
        ])

    def test_an_unbound_helper_and_another_classs_binding_are_no_seam(self):
        # set_cache is bound nowhere; Browser::open shares its name with the bound Other::open
        found = {method for method, _ in params()}
        self.assertNotIn("Browser::set_cache", found)
        self.assertNotIn("Browser::open", found)

    def test_a_commented_out_parameter_is_not_counted(self):
        header = """
class Browser : public RefCounted {
	GDCLASS(Browser, RefCounted)
	void sort_rows(int p_column /*, const Dictionary &p_pings */);
	// void apply_ping_results(const Dictionary &results);
};
"""
        self.assertEqual(params(header), [])


if __name__ == "__main__":
    unittest.main()
