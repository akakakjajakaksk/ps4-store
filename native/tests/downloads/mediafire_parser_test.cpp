#include "../../mediafire_source.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using peppyMediafire::extractUrl;
using peppyMediafire::isCdnUrl;
using peppyMediafire::isPageUrl;

static const char* const kCdn = "https://download123.mediafire.com/token/key/fixture.pkg";

static std::string anchor(const std::string& href = kCdn) {
    return "<a id='downloadButton' href='" + href + "'>download</a>";
}

static void checkPage(const std::string& url, bool expected) {
    size_t origin = 999;
    assert(isPageUrl(url.data(), url.size(), &origin) == expected);
    if (!expected) assert(origin == 0);
    else assert(origin > 8 && origin < url.size() && url[origin] == '/');
}

static void checkCdn(const std::string& url, bool expected) {
    size_t origin = 999;
    assert(isCdnUrl(url.data(), url.size(), &origin) == expected);
    if (!expected) assert(origin == 0);
    else assert(origin > 8 && origin < url.size() && url[origin] == '/');
}

static void checkHtml(const std::string& html, bool expected,
                      const std::string& url = kCdn) {
    static unsigned int testNumber = 0;
    ++testNumber;
    char output[peppyMediafire::URL_CAP];
    memset(output, 0x7f, sizeof(output));
    const bool actual = extractUrl(html.data(), html.size(), output, sizeof(output));
    if (actual != expected) fprintf(stderr, "HTML case %u: expected=%d actual=%d\n", testNumber, expected, actual);
    assert(actual == expected);
    if (expected) assert(std::string(output) == url);
    else assert(output[0] == 0);
}

static void urlTests() {
    checkPage("https://www.mediafire.com/file/aB123/%5BEXAMPLE%5D-Game_v1.00.pkg/file", true);
    checkPage("https://mediafire.com/file/1/[EXAMPLE]-Game%20Name.PKG/file", true);
    checkPage("https://MEDIAFIRE.COM/file/A/Game%2epkg/file", true);
    const char* invalidPages[] = {
        "http://mediafire.com/file/a/game.pkg/file",
        "https://mediafire.com.evil.test/file/a/game.pkg/file",
        "https://evil.test/mediafire.com/file/a/game.pkg/file",
        "https://mediafire.com:443/file/a/game.pkg/file",
        "https://u@mediafire.com/file/a/game.pkg/file",
        "https://www.www.mediafire.com/file/a/game.pkg/file",
        "https://download1.mediafire.com/file/a/game.pkg/file",
        "https://mediafire.com/file//game.pkg/file",
        "https://mediafire.com/file/a-b/game.pkg/file",
        "https://mediafire.com/file/a/game.zip/file",
        "https://mediafire.com/file/a/.pkg/file",
        "https://mediafire.com/file/a/../file",
        "https://mediafire.com/file/a/%2e%2e/file",
        "https://mediafire.com/file/a/sub%2fgame.pkg/file",
        "https://mediafire.com/file/a/sub%5cgame.pkg/file",
        "https://mediafire.com/file/a/game%00.pkg/file",
        "https://mediafire.com/file/a/game%0a.pkg/file",
        "https://mediafire.com/file/a/game%7f.pkg/file",
        "https://mediafire.com/file/a/game%2.pkg/file",
        "https://mediafire.com/file/a/game%gg.pkg/file",
        "https://mediafire.com/file/a/game.pkg/file?x=1",
        "https://mediafire.com/file/a/game.pkg/file#fragment",
        "https://mediafire.com/file/a/game.pkg/file/",
        "https://mediafire.com/file/a/game name.pkg/file"
    };
    for (size_t i = 0; i < sizeof(invalidPages) / sizeof(invalidPages[0]); ++i)
        checkPage(invalidPages[i], false);
    checkCdn(kCdn, true);
    checkCdn("https://DOWNLOAD0.MEDIAFIRE.COM/a.pkg?key=abc&sig=%2F%20%3D", true);
    const char* invalidCdns[] = {
        "http://download1.mediafire.com/a.pkg",
        "https://download.mediafire.com/a.pkg",
        "https://download1x.mediafire.com/a.pkg",
        "https://download1.mediafire.com.evil.test/a.pkg",
        "https://download1.mediafire.com:443/a.pkg",
        "https://u@download1.mediafire.com/a.pkg",
        "https://www.download1.mediafire.com/a.pkg",
        "https://download1.mediafire.com./a.pkg",
        "https://download1.mediafire.com",
        "https://download1.mediafire.com/",
        "https://download1.mediafire.com/?x=1",
        "https://download1.mediafire.com/a.pkg#x",
        "https://download1.mediafire.com/a\\b.pkg",
        "https://download1.mediafire.com/a%5cb.pkg",
        "https://download1.mediafire.com/a%00.pkg",
        "https://download1.mediafire.com/a%ff%.pkg",
        "https://download1.mediafire.com/a\n.pkg",
        "https://download1.mediafire.com/a\".pkg",
        "https://download1.mediafire.com/a<.pkg"
    };
    for (size_t i = 0; i < sizeof(invalidCdns) / sizeof(invalidCdns[0]); ++i)
        checkCdn(invalidCdns[i], false);
    checkCdn("https://download" + std::string(55, '1') + ".mediafire.com/a", true);
    checkCdn("https://download" + std::string(56, '1') + ".mediafire.com/a", false);
    std::string bounded = "https://download1.mediafire.com/";
    bounded.append(peppyMediafire::URL_CAP - 1 - bounded.size(), 'a');
    checkCdn(bounded, true);
    bounded += 'a';
    checkCdn(bounded, false);
    std::string embeddedNul(kCdn);
    embeddedNul.insert(embeddedNul.size() - 4, 1, '\0');
    checkCdn(embeddedNul, false);
    size_t origin = 1;
    assert(!isPageUrl(0, 0, &origin) && !origin);
    origin = 1;
    assert(!isCdnUrl(0, 100, &origin) && !origin);
    // Explicit spans do not need a terminating NUL and ignore bytes beyond length.
    const char exact[] = {'h','t','t','p','s',':','/','/','d','o','w','n','l','o','a','d','1','.',
                          'm','e','d','i','a','f','i','r','e','.','c','o','m','/','a'};
    assert(isCdnUrl(exact, sizeof(exact)));
}

static void htmlTests() {
    checkHtml(anchor(), true);
    checkHtml("<!doctype html><html><head><title>store</title></head><body>" + anchor() + "</body></html>", true);
    checkHtml("<A CLASS=button disabled HREF=\"" + std::string(kCdn) + "\" ID=\"downloadButton\">x</A>", true);
    checkHtml("<a href='" + std::string(kCdn) + "'\n id = 'downloadButton' />", true);
    const std::string query = std::string(kCdn) + "?x=1&sig=abc&expires=123";
    checkHtml(anchor(std::string(kCdn) + "?x=1&amp;sig=abc&amp;expires=123"), true, query);
    checkHtml(anchor(query), true, query);
    const std::string rawQuery = std::string(kCdn) + "?x=1&signature_1=abc&response-content-type=download";
    checkHtml(anchor(rawQuery), true, rawQuery);
    checkHtml("<!--" + anchor("https://download999.mediafire.com/fake") + "-->" + anchor(), true);
    checkHtml("<script>var a=\"" + anchor() + "\";</ScRiPt>" + anchor(), true);
    checkHtml("<script data-x='>'>" + anchor() + "</scripture>still script</script>" + anchor(), true);
    checkHtml("<script><!--<script>double escaped</script>" + anchor() + "</script>", false);
    checkHtml("<script><!--<script>double escaped</script>" + anchor() + "</script>" + anchor(), true);
    checkHtml("<script><!--<script>double escaped-->" + anchor() + "</script>", false);
    checkHtml("<style>" + anchor() + "</style>" + anchor(), true);
    const char* rawTags[] = {"textarea", "title", "xmp", "iframe", "noembed", "noframes", "noscript"};
    for (size_t i = 0; i < sizeof(rawTags) / sizeof(rawTags[0]); ++i) {
        const std::string name(rawTags[i]);
        checkHtml("<" + name + ">" + anchor() + "</" + name + ">", false);
        checkHtml("<" + name + ">" + anchor() + "</" + name + ">" + anchor(), true);
    }
    checkHtml("<template>" + anchor() + "</template>" + anchor(), true);
    checkHtml("<template><template>" + anchor() + "</template>" + anchor() + "</template>", false);
    checkHtml("<template><script>'</template>'</script>" + anchor() + "</template>" + anchor(), true);
    checkHtml(anchor() + "<template>", false);
    checkHtml(anchor() + "<script>", false);
    checkHtml(anchor() + "<!--", false);
    checkHtml("<plaintext>" + anchor(), false);
    checkHtml("<script>" + anchor() + "</script>", false);
    checkHtml("<!--" + anchor() + "-->", false);
    checkHtml("&lt;a id='downloadButton' href='" + std::string(kCdn) + "'&gt;", false);
    checkHtml("<div id='downloadButton' href='" + std::string(kCdn) + "'></div>", false);
    checkHtml("<a id='DownloadButton' href='" + std::string(kCdn) + "'>x</a>", false);
    checkHtml("<a id=downloadButton href='" + std::string(kCdn) + "'>x</a>", false);
    checkHtml("<a id='downloadButton' href=" + std::string(kCdn) + ">x</a>", false);
    checkHtml("<a id='downloadButton'>x</a>", false);
    checkHtml("<a id='downloadButton' href='" + std::string(kCdn) + "' href='" + kCdn + "'>", false);
    checkHtml("<a id='downloadButton' ID='downloadButton' href='" + std::string(kCdn) + "'>", false);
    checkHtml("<a id='downloadButton'href='" + std::string(kCdn) + "'>", false);
    checkHtml("<a id='downloadButton' href='" + std::string(kCdn), false);
    checkHtml(anchor() + anchor(), false);
    checkHtml(anchor() + anchor("https://download999.mediafire.com/other"), false);
    checkHtml(anchor("https://evil.test/fixture.pkg"), false);
    checkHtml(anchor(std::string(kCdn) + "?x=&quot;"), false);
    checkHtml(anchor(std::string(kCdn) + "?x=&#47;"), false);
    checkHtml(anchor(std::string(kCdn) + "?x=&#x2f;"), false);
    checkHtml(anchor(std::string(kCdn) + "?x=&AMP;"), false);
    checkHtml(anchor(std::string(kCdn) + "?x=&copy"), false);
    checkHtml(anchor(std::string(kCdn) + "?x=&amp;amp;"), true, std::string(kCdn) + "?x=&amp;");
    checkHtml(anchor(std::string(kCdn) + "?x=1&copy=2"), true, std::string(kCdn) + "?x=1&copy=2");

    std::string full(peppyMediafire::HTML_CAP - anchor().size(), ' ');
    full += anchor();
    checkHtml(full, true);
    full += ' ';
    checkHtml(full, false);
    std::string nul = anchor();
    nul.insert(0, 1, '\0');
    checkHtml(nul, false);

    struct Guard { char before; char output[16]; char after; } guard;
    memset(&guard, 0x4a, sizeof(guard));
    const std::string html = anchor();
    assert(!extractUrl(html.data(), html.size(), guard.output, sizeof(guard.output)));
    assert(guard.output[0] == 0 && guard.before == 0x4a && guard.after == 0x4a);
    std::vector<char> exact(strlen(kCdn) + 1, '\0');
    assert(extractUrl(html.data(), html.size(), exact.data(), exact.size()));
    assert(!extractUrl(html.data(), html.size(), exact.data(), exact.size() - 1));
    assert(!exact[0]);
    char empty = 'x';
    assert(!extractUrl(0, 1, &empty, 1) && !empty);
    empty = 'x';
    assert(!extractUrl(html.data(), html.size(), &empty, 0) && empty == 'x');
    assert(!extractUrl(html.data(), html.size(), 0, 1));

    // Every truncated prefix is bounded, including prefixes inside quoted hrefs.
    const std::string mixed = "<!--fake--><script>x</script>" + html;
    for (size_t i = 0; i < mixed.size(); ++i) {
        char output[peppyMediafire::URL_CAP];
        const bool ok = extractUrl(mixed.data(), i, output, sizeof(output));
        if (ok) assert(isCdnUrl(output, strlen(output)));
        else assert(!output[0]);
    }
    // Deterministic malformed-input exercise under the sanitizer builds.
    unsigned int state = 0x5164a91u;
    const char alphabet[] = "<>=/!'\"&;# %abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\n\t";
    for (size_t test = 0; test < 4000; ++test) {
        std::string random(test % 257, ' ');
        for (size_t j = 0; j < random.size(); ++j) {
            state = state * 1664525u + 1013904223u;
            random[j] = alphabet[state % (sizeof(alphabet) - 1)];
        }
        char output[peppyMediafire::URL_CAP];
        const bool ok = extractUrl(random.data(), random.size(), output, sizeof(output));
        if (ok) assert(isCdnUrl(output, strlen(output)));
        else assert(!output[0]);
    }
}

int main(int argc, char** argv) {
    urlTests();
    htmlTests();
    if (argc == 2) {
        std::ifstream file(argv[1], std::ios::binary);
        assert(file.good());
        const std::string html((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        char output[peppyMediafire::URL_CAP];
        const bool ok = extractUrl(html.data(), html.size(), output, sizeof(output));
        printf("saved HTML: accepted=%d bytes=%zu url_length=%zu\n", ok ? 1 : 0,
               html.size(), ok ? strlen(output) : size_t(0));
        assert(ok);
    }
    puts("MediaFire parser tests passed");
    return 0;
}
