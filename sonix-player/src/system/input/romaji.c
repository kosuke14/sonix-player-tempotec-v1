#include "romaji.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// One romaji spelling and the hiragana it completes to -- one to three code
// points, UTF-8. Katakana is never written down: it is the same code points a
// fixed distance up (0x3041-0x3096 and their 0x30A1-30F6 twins, which holds
// for every small kana and ゔ too), so one rule carries the shift key.
typedef struct {
	const char *rom;
	const char *hira;
} romaji_map_t;

// The romanisations a Japanese IME accepts, minus the ones that are rules
// rather than spellings: ん comes from n before a consonant (the "nn" of
// "konnichi" and the "nk" of "bank" are the same rule), and the っ of a
// doubled consonant ("gakkou") comes from the repeat itself. There is
// therefore no bare "n" and no bare consonant here -- an entry that matched
// alone would preempt both.
static const romaji_map_t ROMAJI[] = {
	{"a", "\xe3\x81\x82"},		 // あ
	{"i", "\xe3\x81\x84"},		 // い
	{"u", "\xe3\x81\x86"},		 // う
	{"e", "\xe3\x81\x88"},		 // え
	{"o", "\xe3\x81\x8a"},		 // お
	{"ka", "\xe3\x81\x8b"},		 // か
	{"ki", "\xe3\x81\x8d"},		 // き
	{"ku", "\xe3\x81\x8f"},		 // く
	{"ke", "\xe3\x81\x91"},		 // け
	{"ko", "\xe3\x81\x93"},		 // こ
	{"ga", "\xe3\x81\x8c"},		 // が
	{"gi", "\xe3\x81\x8e"},		 // ぎ
	{"gu", "\xe3\x81\x90"},		 // ぐ
	{"ge", "\xe3\x81\x92"},		 // げ
	{"go", "\xe3\x81\x94"},		 // ご
	{"sa", "\xe3\x81\x95"},		 // さ
	{"si", "\xe3\x81\x97"},		 // し
	{"shi", "\xe3\x81\x97"},	 // し
	{"su", "\xe3\x81\x99"},		 // す
	{"se", "\xe3\x81\x9b"},		 // せ
	{"so", "\xe3\x81\x9d"},		 // そ
	{"za", "\xe3\x81\x96"},		 // ざ
	{"ji", "\xe3\x81\x98"},		 // じ
	{"zi", "\xe3\x81\x98"},		 // じ
	{"zu", "\xe3\x81\x9a"},		 // ず
	{"ze", "\xe3\x81\x9c"},		 // ぜ
	{"zo", "\xe3\x81\x9e"},		 // ぞ
	{"ta", "\xe3\x81\x9f"},		 // た
	{"ti", "\xe3\x81\xa1"},		 // ち
	{"chi", "\xe3\x81\xa1"},	 // ち
	{"tu", "\xe3\x81\xa4"},		 // つ
	{"tsu", "\xe3\x81\xa4"},	 // つ
	{"te", "\xe3\x81\xa6"},		 // て
	{"to", "\xe3\x81\xa8"},		 // と
	{"da", "\xe3\x81\xa0"},		 // だ
	{"di", "\xe3\x81\xa2"},		 // ぢ
	{"du", "\xe3\x81\xa5"},		 // づ
	{"de", "\xe3\x81\xa7"},		 // で
	{"do", "\xe3\x81\xa9"},		 // ど
	{"na", "\xe3\x81\xaa"},		 // な
	{"ni", "\xe3\x81\xab"},		 // に
	{"nu", "\xe3\x81\xac"},		 // ぬ
	{"ne", "\xe3\x81\xad"},		 // ね
	{"no", "\xe3\x81\xae"},		 // の
	{"ha", "\xe3\x81\xaf"},		 // は
	{"hi", "\xe3\x81\xb2"},		 // ひ
	{"hu", "\xe3\x81\xb5"},		 // ふ
	{"fu", "\xe3\x81\xb5"},		 // ふ
	{"he", "\xe3\x81\xb8"},		 // へ
	{"ho", "\xe3\x81\xbb"},		 // ほ
	{"ba", "\xe3\x81\xb0"},		 // ば
	{"bi", "\xe3\x81\xb3"},		 // び
	{"bu", "\xe3\x81\xb6"},		 // ぶ
	{"be", "\xe3\x81\xb9"},		 // べ
	{"bo", "\xe3\x81\xbc"},		 // ぼ
	{"pa", "\xe3\x81\xb1"},		 // ぱ
	{"pi", "\xe3\x81\xb4"},		 // ぴ
	{"pu", "\xe3\x81\xb7"},		 // ぷ
	{"pe", "\xe3\x81\xba"},		 // ぺ
	{"po", "\xe3\x81\xbd"},		 // ぽ
	{"ma", "\xe3\x81\xbe"},		 // ま
	{"mi", "\xe3\x81\xbf"},		 // み
	{"mu", "\xe3\x82\x80"},		 // む
	{"me", "\xe3\x82\x81"},		 // め
	{"mo", "\xe3\x82\x82"},		 // も
	{"ya", "\xe3\x82\x84"},		 // や
	{"yu", "\xe3\x82\x86"},		 // ゆ
	{"yo", "\xe3\x82\x88"},		 // よ
	{"ye", "\xe3\x81\x84\xe3\x81\x87"}, // いぇ
	{"ra", "\xe3\x82\x89"},		 // ら
	{"ri", "\xe3\x82\x8a"},		 // り
	{"ru", "\xe3\x82\x8b"},		 // る
	{"re", "\xe3\x82\x8c"},		 // れ
	{"ro", "\xe3\x82\x8d"},		 // ろ
	{"wa", "\xe3\x82\x8f"},		 // わ
	{"wi", "\xe3\x81\x86\xe3\x81\x83"}, // うぃ
	{"we", "\xe3\x81\x86\xe3\x81\x87"}, // うぇ
	{"wo", "\xe3\x82\x92"},		 // を
	// Foreign sounds, built on the small kana.
	{"fa", "\xe3\x81\xb5\xe3\x81\x81"}, // ふぁ
	{"fi", "\xe3\x81\xb5\xe3\x81\x83"}, // ふぃ
	{"fe", "\xe3\x81\xb5\xe3\x81\x87"}, // ふぇ
	{"fo", "\xe3\x81\xb5\xe3\x81\x89"}, // ふぉ
	{"va", "\xe3\x82\x94\xe3\x81\x81"}, // ゔぁ
	{"vi", "\xe3\x82\x94\xe3\x81\x83"}, // ゔぃ
	{"vu", "\xe3\x82\x94"},		 // ゔ
	{"ve", "\xe3\x82\x94\xe3\x81\x87"}, // ゔぇ
	{"vo", "\xe3\x82\x94\xe3\x81\x89"}, // ゔぉ
	{"tsa", "\xe3\x81\xa4\xe3\x81\x81"}, // つぁ
	{"tsi", "\xe3\x81\xa4\xe3\x81\x83"}, // つぃ
	{"tse", "\xe3\x81\xa4\xe3\x81\x87"}, // つぇ
	{"tso", "\xe3\x81\xa4\xe3\x81\x89"}, // つぉ
	// The y-digraphs, in every romanisation they carry.
	{"kya", "\xe3\x81\x8d\xe3\x82\x83"}, // きゃ
	{"kyu", "\xe3\x81\x8d\xe3\x82\x85"}, // きゅ
	{"kyo", "\xe3\x81\x8d\xe3\x82\x87"}, // きょ
	{"gya", "\xe3\x81\x8e\xe3\x82\x83"}, // ぎゃ
	{"gyu", "\xe3\x81\x8e\xe3\x82\x85"}, // ぎゅ
	{"gyo", "\xe3\x81\x8e\xe3\x82\x87"}, // ぎょ
	{"sha", "\xe3\x81\x97\xe3\x82\x83"}, // しゃ
	{"shu", "\xe3\x81\x97\xe3\x82\x85"}, // しゅ
	{"sho", "\xe3\x81\x97\xe3\x82\x87"}, // しょ
	{"sya", "\xe3\x81\x97\xe3\x82\x83"}, // しゃ
	{"syu", "\xe3\x81\x97\xe3\x82\x85"}, // しゅ
	{"syo", "\xe3\x81\x97\xe3\x82\x87"}, // しょ
	{"ja", "\xe3\x81\x98\xe3\x82\x83"},  // じゃ
	{"ju", "\xe3\x81\x98\xe3\x82\x85"},  // じゅ
	{"jo", "\xe3\x81\x98\xe3\x82\x87"},  // じょ
	{"jya", "\xe3\x81\x98\xe3\x82\x83"}, // じゃ
	{"jyu", "\xe3\x81\x98\xe3\x82\x85"}, // じゅ
	{"jyo", "\xe3\x81\x98\xe3\x82\x87"}, // じょ
	{"cha", "\xe3\x81\xa1\xe3\x82\x83"}, // ちゃ
	{"chu", "\xe3\x81\xa1\xe3\x82\x85"}, // ちゅ
	{"cho", "\xe3\x81\xa1\xe3\x82\x87"}, // ちょ
	{"cya", "\xe3\x81\xa1\xe3\x82\x83"}, // ちゃ
	{"cyu", "\xe3\x81\xa1\xe3\x82\x85"}, // ちゅ
	{"cyo", "\xe3\x81\xa1\xe3\x82\x87"}, // ちょ
	{"tya", "\xe3\x81\xa1\xe3\x82\x83"}, // ちゃ
	{"tyu", "\xe3\x81\xa1\xe3\x82\x85"}, // ちゅ
	{"tyo", "\xe3\x81\xa1\xe3\x82\x87"}, // ちょ
	{"dya", "\xe3\x81\xa2\xe3\x82\x83"}, // ぢゃ
	{"dyu", "\xe3\x81\xa2\xe3\x82\x85"}, // ぢゅ
	{"dyo", "\xe3\x81\xa2\xe3\x82\x87"}, // ぢょ
	{"nya", "\xe3\x81\xab\xe3\x82\x83"}, // にゃ
	{"nyu", "\xe3\x81\xab\xe3\x82\x85"}, // にゅ
	{"nyo", "\xe3\x81\xab\xe3\x82\x87"}, // にょ
	{"hya", "\xe3\x81\xb2\xe3\x82\x83"}, // ひゃ
	{"hyu", "\xe3\x81\xb2\xe3\x82\x85"}, // ひゅ
	{"hyo", "\xe3\x81\xb2\xe3\x82\x87"}, // ひょ
	{"bya", "\xe3\x81\xb3\xe3\x82\x83"}, // びゃ
	{"byu", "\xe3\x81\xb3\xe3\x82\x85"}, // びゅ
	{"byo", "\xe3\x81\xb3\xe3\x82\x87"}, // びょ
	{"pya", "\xe3\x81\xb4\xe3\x82\x83"}, // ぴゃ
	{"pyu", "\xe3\x81\xb4\xe3\x82\x85"}, // ぴゅ
	{"pyo", "\xe3\x81\xb4\xe3\x82\x87"}, // ぴょ
	{"mya", "\xe3\x81\xbf\xe3\x82\x83"}, // みゃ
	{"myu", "\xe3\x81\xbf\xe3\x82\x85"}, // みゅ
	{"myo", "\xe3\x81\xbf\xe3\x82\x87"}, // みょ
	{"rya", "\xe3\x82\x8a\xe3\x82\x83"}, // りゃ
	{"ryu", "\xe3\x82\x8a\xe3\x82\x85"}, // りゅ
	{"ryo", "\xe3\x82\x8a\xe3\x82\x87"}, // りょ
	// The small kana, behind x or l the way an IME spells them.
	{"xa", "\xe3\x81\x81"},		 // ぁ
	{"xi", "\xe3\x81\x83"},		 // ぃ
	{"xu", "\xe3\x81\x85"},		 // ぅ
	{"xe", "\xe3\x81\x87"},		 // ぇ
	{"xo", "\xe3\x81\x89"},		 // ぉ
	{"la", "\xe3\x81\x81"},		 // ぁ
	{"li", "\xe3\x81\x83"},		 // ぃ
	{"lu", "\xe3\x81\x85"},		 // ぅ
	{"le", "\xe3\x81\x87"},		 // ぇ
	{"lo", "\xe3\x81\x89"},		 // ぉ
	{"xtu", "\xe3\x81\xa3"},	 // っ
	{"ltu", "\xe3\x81\xa3"},	 // っ
	{"xya", "\xe3\x82\x83"},	 // ゃ
	{"xyu", "\xe3\x82\x85"},	 // ゅ
	{"xyo", "\xe3\x82\x87"},	 // ょ
	{"lya", "\xe3\x82\x83"},	 // ゃ
	{"lyu", "\xe3\x82\x85"},	 // ゅ
	{"lyo", "\xe3\x82\x87"},	 // ょ
};

// ん and っ, the two kana that come from rules rather than spellings.
#define KANA_N "\xe3\x82\x93"	 // ん
#define KANA_TSU "\xe3\x81\xa3" // っ

// The hiragana block and its katakana twin; ー (0x30FC) and the punctuation
// sit outside it, which is what keeps the rule from touching them.
#define HIRAGANA_FIRST 0x3041
#define HIRAGANA_LAST 0x3096
#define KATAKANA_STEP 0x60

static bool is_consonant(char c) {
	if (c < 'a' || c > 'z') {
		return false;
	}
	return c != 'a' && c != 'i' && c != 'u' && c != 'e' && c != 'o' && c != 'y';
}

// The longest spelling that starts `s`: three letters, then two, then one.
static const char *match_at(const char *s, int *len) {
	size_t have = strlen(s);
	for (int l = 3; l >= 1; l--) {
		if ((int)have < l) {
			continue;
		}
		for (size_t i = 0; i < sizeof(ROMAJI) / sizeof(ROMAJI[0]); i++) {
			if (strlen(ROMAJI[i].rom) == (size_t)l && strncmp(ROMAJI[i].rom, s, (size_t)l) == 0) {
				*len = l;
				return ROMAJI[i].hira;
			}
		}
	}
	return NULL;
}

// Reads one UTF-8 code point. Every kana here is a 3-byte sequence, so the
// caller only ever asks about the starts it handed over itself.
static uint32_t code_point_of(const char *p) {
	const unsigned char *u = (const unsigned char *)p;
	if ((*u & 0xE0) != 0xE0) {
		return 0;
	}
	return ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F);
}

// Appends one kana to `emit`, in the script `katakana` asks for.
static void emit_kana(char *emit, size_t emit_size, const char *hira, bool katakana) {
	if (!emit || !emit_size) {
		return;
	}
	size_t used = strlen(emit);
	for (const char *p = hira; *p && used + 4 <= emit_size; p += 3) {
		uint32_t cp = code_point_of(p);
		if (katakana && cp >= HIRAGANA_FIRST && cp <= HIRAGANA_LAST) {
			cp += KATAKANA_STEP;
		}
		emit[used++] = (char)(0xE0 | (cp >> 12));
		emit[used++] = (char)(0x80 | ((cp >> 6) & 0x3F));
		emit[used++] = (char)(0x80 | (cp & 0x3F));
	}
	emit[used] = '\0';
}

static int feed(const char *seq, bool katakana, char *emit, size_t emit_size) {
	int pos = 0;
	if (emit && emit_size) {
		emit[0] = '\0';
	}
	while (seq[pos]) {
		int len = 0;
		const char *kana = match_at(seq + pos, &len);
		if (!kana) {
			char c0 = seq[pos], c1 = seq[pos + 1];
			if (c0 == 'n' && is_consonant(c1)) {
				// "n" before a consonant closes its own syllable: the nn of
				// "konnichi", the nb of "shinbun". One n goes, and a second
				// one stays to begin whatever follows.
				kana = KANA_N;
				len = 1;
			} else if (c0 == c1 && is_consonant(c0)) {
				// A doubled consonant types the っ first: the kk of
				// "gakkou". The "ny" of "ponyo" is exempt -- y is not a
				// consonant here -- and so is "nn", which is the rule above.
				kana = KANA_TSU;
				len = 1;
			} else {
				break; // nothing completes here; the tail keeps waiting
			}
		}
		emit_kana(emit, emit_size, kana, katakana);
		pos += len;
	}
	return pos;
}

int romaji_tail(const char *tail, bool katakana, char *emit, size_t emit_size) {
	return feed(tail ? tail : "", katakana, emit, emit_size);
}

int romaji_feed(const char *pending, char c, bool katakana, char *emit, size_t emit_size) {
	char seq[ROMAJI_TAIL_MAX + 2];
	snprintf(seq, sizeof(seq), "%.4s%c", pending ? pending : "", c);
	return feed(seq, katakana, emit, emit_size);
}

int romaji_flush(const char *pending, bool katakana, char *emit, size_t emit_size) {
	if (!pending || pending[0] != 'n' || pending[1]) {
		return 0;
	}
	if (emit && emit_size) {
		emit[0] = '\0';
	}
	emit_kana(emit, emit_size, KANA_N, katakana);
	return 1;
}
