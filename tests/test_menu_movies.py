"""menu-movies source discovery + graceful degradation (no ffmpeg needed)."""

from pathlib import Path

from apps.importer.menu_movies import (
    MENU_MOVIE_NAMES,
    convert_menu_movies,
    find_menu_movie_sources,
)


def _touch(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"BIKi")


def test_find_sources_covers_root_and_expansions(tmp_path: Path) -> None:
    _touch(tmp_path / "main.bik")
    _touch(tmp_path / "footer.bik")
    _touch(tmp_path / "expansion" / "jox01" / "main.bik")
    _touch(tmp_path / "expansion" / "jox01" / "header.bik")
    # Noise that must not be picked up.
    _touch(tmp_path / "intro.bik")
    _touch(tmp_path / "expansion" / "jox01" / "notes.txt")

    found = find_menu_movie_sources(tmp_path)

    rels = sorted(p.relative_to(tmp_path).as_posix() for p in found)
    assert rels == [
        "expansion/jox01/header.bik",
        "expansion/jox01/main.bik",
        "footer.bik",
        "main.bik",
    ]


def test_slot_names_match_the_engine_table() -> None:
    assert MENU_MOVIE_NAMES == ("main.bik", "header.bik", "footer.bik")


def test_convert_reports_missing_movies(tmp_path: Path, capsys) -> None:
    assert convert_menu_movies(tmp_path) == 1
    out = capsys.readouterr().out
    assert "No menu movies" in out


def test_convert_without_ffmpeg_degrades_with_guidance(
    tmp_path: Path, capsys, monkeypatch
) -> None:
    _touch(tmp_path / "main.bik")
    monkeypatch.setattr(
        "apps.importer.menu_movies._ffmpeg_exe", lambda: None
    )
    assert convert_menu_movies(tmp_path) == 1
    out = capsys.readouterr().out
    assert "imageio-ffmpeg" in out
