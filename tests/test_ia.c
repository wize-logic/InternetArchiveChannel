/* Host unit test: archive.org URL building, JSON parsing and file selection on real fixtures
 * (tests/fixtures, fetched 2026-10-09). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ia/ia.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc((size_t)n + 1);
	size_t got = fread(buf, 1, (size_t)n, f);
	fclose(f);
	buf[got] = '\0';
	return buf;
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "tests/fixtures";
	char path[512], url[2048];

	/* URLs */
	CHECK(ia_search_url(url, sizeof url, "collection:prelinger", IA_SORT_POPULAR, 12, 1) == 0);
	CHECK(strstr(url, "q=%28collection%3Aprelinger%29%20AND%20NOT%20access-restricted-item%3Atrue") != NULL);
	CHECK(strstr(url, "sort%5B%5D=downloads+desc&rows=12&page=1&output=json") != NULL);
	CHECK(strstr(url, "NOT%20collection%3A%28inlibrary%20OR%20printdisabled%20OR%20stream_only%20OR%20tvarchive") != NULL);
	char q[512];
	ia_user_query("grateful dead: live*", "audio", 1, q, sizeof q);
	CHECK(strcmp(q, "(title:(grateful AND dead AND live) OR creator:(grateful AND dead AND live)) AND "
		"mediatype:(audio OR etree) AND NOT identifier:fav-*") == 0);
	ia_user_query("  \"night of\" OR dead ", "", 0, q, sizeof q);
	CHECK(strcmp(q, "(\"night of\" AND or AND dead) AND mediatype:(movies OR audio OR etree OR texts OR image OR "
		"collection) AND NOT identifier:fav-*") == 0);
	ia_user_query("movies", "movies", 0, q, sizeof q);
	CHECK(strcmp(q, "(movies) AND mediatype:movies AND NOT identifier:fav-*") == 0);
	ia_user_query(" ?! \"\" ", "", 1, q, sizeof q);
	CHECK(q[0] == '\0');
	CHECK(ia_thumb_url(url, sizeof url, "gd1977-05-08") == 0 &&
		strcmp(url, "https://archive.org/services/img/gd1977-05-08") == 0);
	CHECK(ia_file_url(url, sizeof url, "my item", "dir/My File #1.mp3") == 0 &&
		strcmp(url, "https://archive.org/download/my%20item/dir/My%20File%20%231.mp3") == 0);

	CHECK(ia_book_page_url(url, sizeof url, "alicesadventures00carr", 5, 700) == 0 &&
		strcmp(url, "https://archive.org/download/alicesadventures00carr/page/n5_w700.jpg") == 0);
	CHECK(ia_iiif_url(url, sizeof url, "4731908937_d9d5cae8dc_o", "4731908937_d9d5cae8dc_o.jpg", 1024, 768) == 0 &&
		strcmp(url, "https://iiif.archive.org/image/iiif/3/4731908937_d9d5cae8dc_o%2F4731908937_d9d5cae8dc_o.jpg"
			    "/full/!1024,768/0/default.jpg") == 0);
	CHECK(ia_iiif_url(url, sizeof url, "cinema-hd_20210711", "CinemaHD.png", 0, 0) == 0 &&
		strcmp(url, "https://iiif.archive.org/image/iiif/3/cinema-hd_20210711%2FCinemaHD.png/full/max/0/default.jpg") == 0);

	char ts[32];
	CHECK(ia_wayback_available_url(url, sizeof url, "www.yahoo.com", "1999") == 0 &&
		strcmp(url, "https://archive.org/wayback/available?url=www.yahoo.com&timestamp=1999") == 0);
	CHECK(ia_parse_wayback_available("{\"url\":\"yahoo.com\",\"archived_snapshots\":{\"closest\":"
		"{\"status\":\"200\",\"available\":true,\"url\":\"http://web.archive.org/web/19990117032727/"
		"http://www.yahoo.com:80/\",\"timestamp\":\"19990117032727\"}}}", ts, sizeof ts) == 0 &&
		strcmp(ts, "19990117032727") == 0);
	CHECK(ia_parse_wayback_available("{\"url\":\"x\",\"archived_snapshots\":{}}", ts, sizeof ts) != 0);
	{
		char cdx[1024], tss[4][IA_TS_LEN];
		CHECK(ia_wayback_cdx_url(cdx, sizeof cdx, "www.yahoo.com", 1999) == 0 &&
			strstr(cdx, "url=www.yahoo.com&from=1999&to=1999&output=json") != NULL);
		CHECK(ia_parse_cdx("[[\"timestamp\"],[\"19990117083422\"],[\"19990125092002\"]]", tss, 4) == 2 &&
			strcmp(tss[0], "19990117083422") == 0 && strcmp(tss[1], "19990125092002") == 0);
		CHECK(ia_parse_cdx("[]", tss, 4) == 0);
		CHECK(ia_parse_cdx("19990117083422 200\n19990301000000 200\n", tss, 4) == 2 &&
			strcmp(tss[1], "19990301000000") == 0);
		CHECK(ia_parse_cdx("[[\"timestamp\"],[\"19990101000000\"],[\"19990102000000\"],[\"19990103000000\"]]", tss, 2) == 2);
		CHECK(ia_parse_cdx("{\"error\":1}", tss, 4) == -1);
	}

	/* Search */
	snprintf(path, sizeof path, "%s/search_prelinger.json", dir);
	char *json = slurp(path);
	CHECK(json != NULL);
	struct ia_search s;
	CHECK(json && ia_parse_search(json, &s) == 0);
	CHECK(s.count == 12 && s.total > 10000);
	CHECK(strcmp(s.docs[0].id, "AboutBan1935") == 0 && strcmp(s.docs[0].title, "About Bananas") == 0);
	CHECK(strcmp(s.docs[0].year, "1935") == 0 && strcmp(s.docs[0].creator, "Castle Films") == 0);
	ia_search_free(&s);
	free(json);
	CHECK(ia_parse_search("{\"response\":{}}", &s) != 0);
	CHECK(ia_parse_search("not json", &s) != 0);

	/* Movie item: the 512Kb derivative wins over h.264/Ogg/HiRes. */
	snprintf(path, sizeof path, "%s/item_122Eyes1950.json", dir);
	json = slurp(path);
	struct ia_item it;
	CHECK(json && ia_parse_item(json, &it) == 0);
	CHECK(strcmp(it.id, "122Eyes1950") == 0 && it.media == IA_MEDIA_MOVIES && it.file_count == 43);
	int fast = -1, v = ia_pick_video(&it, &fast);
	CHECK(v >= 0 && strcmp(it.files[v].name, "122Eyes1950_512kb.mp4") == 0 && fast == 0);
	CHECK(it.description && !strchr(it.description, '<'));
	ia_item_free(&it);
	free(json);

	/* Concert: 23 VBR MP3 tracks preferred over Ogg/Flac. */
	snprintf(path, sizeof path, "%s/item_gd1977.json", dir);
	json = slurp(path);
	CHECK(json && ia_parse_item(json, &it) == 0);
	CHECK(it.media == IA_MEDIA_AUDIO);
	int tracks[64], nt = ia_list_audio(&it, tracks, 64);
	CHECK(nt == 23 && strcmp(it.files[tracks[0]].format, "VBR MP3") == 0);
	/* "length" comes as "m:ss" strings here: Minglewood Blues is several minutes, not 5 s. */
	CHECK(it.files[tracks[1]].length > 120);
	CHECK(ia_pick_video(&it, NULL) == -1);
	ia_item_free(&it);
	free(json);
	CHECK(ia_parse_item("{}", &it) != 0);

	/* HTML stripping */
	char html[] = "A <b>bold</b>&nbsp;claim.<br/>Next &amp; last<p>para</p>  \n\n  end";
	ia_strip_html(html);
	CHECK(strcmp(html, "A bold claim.\nNext & last\npara\nend") == 0);
	if (failures)
		printf("stripped: [%s]\n", html);

	/* Books: a PDF upload has page images (a _jp2.zip) but no imagecount; the page count comes
	 * from its _page_numbers.json (or _scandata.xml). */
	snprintf(path, sizeof path, "%s/item_pdf_upload.json", dir);
	json = slurp(path);
	CHECK(json && ia_parse_item(json, &it) == 0);
	CHECK(it.media == IA_MEDIA_TEXTS && it.imagecount == 0 && ia_has_book_pages(&it));
	int pc = ia_find_page_count_file(&it);
	CHECK(pc >= 0 && strcmp(it.files[pc].name, "Atomic habits ( PDFDrive )_page_numbers.json") == 0);
	int tx = ia_find_text(&it);
	CHECK(tx >= 0 && strcmp(it.files[tx].name, "Atomic habits ( PDFDrive )_djvu.txt") == 0);
	ia_item_free(&it);
	free(json);
	snprintf(path, sizeof path, "%s/goody_page_numbers.json", dir);
	json = slurp(path);
	CHECK(json && ia_parse_page_count(json) == 16);
	free(json);
	CHECK(ia_parse_page_count("<book><pageData><page leafNum=\"0\"><addToAccessFormats>false</addToAccessFormats>"
		"</page><page leafNum=\"1\"><addToAccessFormats>true</addToAccessFormats></page>"
		"<page leafNum=\"2\"></page></pageData></book>") == 2);
	CHECK(ia_parse_page_count("<html>Not found</html>") == -1);
	/* A Gutenberg item: no pages; the plain .txt is preferred over the Latin-1 -8.txt. */
	CHECK(ia_parse_item("{\"metadata\":{\"identifier\":\"g\",\"mediatype\":\"texts\"},\"files\":["
		"{\"name\":\"16713-h/16713-h.htm\",\"format\":\"Hypertext\"},{\"name\":\"16713-8.txt\",\"format\":\"Text\"},"
		"{\"name\":\"16713-8.zip\",\"format\":\"ZIP\"},{\"name\":\"16713.txt\",\"format\":\"Text\"}]}", &it) == 0);
	CHECK(!ia_has_book_pages(&it) && ia_find_page_count_file(&it) == -1);
	tx = ia_find_text(&it);
	CHECK(tx >= 0 && strcmp(it.files[tx].name, "16713.txt") == 0);
	ia_item_free(&it);

	/* Text reflow: OCR lines rejoined into paragraphs, hyphenation undone, spaces collapsed. */
	snprintf(path, sizeof path, "%s/goody_djvu.txt", dir);
	json = slurp(path);
	char *flow = json ? ia_reflow_text(json, strlen(json)) : NULL;
	CHECK(flow && strstr(flow, "17ARMER MEANWELL was at one time a very rich man. He owned large fields, "
				   "and had fine flocks of sheep, and plenty of money. But all at once his good fortune "
				   "seemed to desert him.") != NULL);
	CHECK(flow && strstr(flow, "worked on the farm.\n\nAt last he had to sell his farm") != NULL);
	CHECK(flow && strstr(flow, "LOS ANGELES\n\n") != NULL && strstr(flow, "  ") == NULL);
	if (failures && flow)
		printf("reflowed: [%.600s]\n", flow);
	free(flow);
	free(json);
	static const char latin1[] = "Caf\xe9 au lait\r\n\fNext page", bom[] = "\xef\xbb\xbfna\xc3\xafve";
	flow = ia_reflow_text(latin1, sizeof latin1 - 1);
	CHECK(flow && strcmp(flow, "Caf\xc3\xa9 au lait\n\nNext page") == 0);
	free(flow);
	flow = ia_reflow_text(bom, sizeof bom - 1);
	CHECK(flow && strcmp(flow, "na\xc3\xafve") == 0);
	free(flow);

	/* No length cap; equally ranked files go lowest bitrate first, and the bitrate is size/length. */
	struct ia_file vf[3] = {{.format = "MPEG4", .size = 3920604928LL, .length = 12707, .width = 640, .height = 480},
		{.format = "h.264", .size = 748401304LL, .length = 7240, .width = 640, .height = 480},
		{.format = "h.264", .length = 7240, .width = 640, .height = 480}};
	struct ia_item long_it = {.files = vf, .file_count = 3};
	CHECK(ia_file_kbps(&vf[0]) == 2468 && ia_file_kbps(&vf[1]) == 826 && ia_file_kbps(&vf[2]) == 0);
	CHECK(ia_pick_video(&long_it, NULL) == 1);
	long_it.file_count = 1;
	CHECK(ia_pick_video(&long_it, NULL) == 0);

	printf("%s\n", failures ? "FAILED" : "ok");
	return failures != 0;
}
