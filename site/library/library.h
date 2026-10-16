/* The library the public site's preview shows (build_preview.py --library
 * site/library). Every song, artist, album and playlist here is made up, and
 * the covers are the preview's generated stand-ins: no real music, art or
 * sound on the site. */

static const mock_track TRACKS[] = {
    { "Neon Tide", "The Velvet Static", "Afterglow FM", 241000, 0xFF6D28D9u },
    { "Low Orbit", "The Velvet Static", "Afterglow FM", 198000, 0xFF6D28D9u },
    { "Paper Satellites", "Mira Kestrel", "Small Hours", 263000, 0xFF0E7490u },
    { "Glass Harbor", "Mira Kestrel", "Small Hours", 215000, 0xFF0E7490u },
    { "Pixel Rain", "Bitcrush Choir", "Memory Stick", 176000, 0xFF15803Du },
    { "Save Point", "Bitcrush Choir", "Memory Stick", 204000, 0xFF15803Du },
    { "Cardinal Drift", "Oslo Parade", "Northbound", 232000, 0xFFB45309u },
    { "Ay Işığı Otobanı", "Gece Yarısı Kulübü", "Eskişehir Ekspresi", 251000, 0xFFBE185Du },
    { "Çay Bahçesi", "Gece Yarısı Kulübü", "Eskişehir Ekspresi", 189000, 0xFFA16207u },
    { "Hollow Lights", "Juniper Vale", "Weatherglass", 287000, 0xFF1D4ED8u },
};

static const mock_playlist PLAYLISTS[] = {
    { "Night drive", 0xFF3B4FD8u }, { "Lo-fi homebrew", 0xFF0F766Eu }, { "Gym", 0xFFB45309u },
    { "Old PSP days", 0xFF9D174Du }, { "Türkçe pop", 0xFFA16207u },
    { "Focus", 0xFF475569u }, { "Road trip 2026", 0xFF15803Du },
    { "A very long playlist name that does not fit on the PSP screen", 0xFF7C3AEDu },
};

static const char *const QUERIES[] = { "velvet static", "mira kestrel", "gece yarısı" };
