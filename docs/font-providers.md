# Nguồn tải font

`IFontProvider` mô tả nguồn đã được kiểm tra; `FontDownloadService` chịu trách
nhiệm mạng, giới hạn dung lượng/thời gian/redirect, hủy, kiểm tra font và cài đặt
nguyên tử. Provider chỉ nhận tên font, không nhận nội dung hay đường dẫn PDF.

## Thứ tự và trách nhiệm

Thứ tự resolve là font nhúng trong PDF → font hệ thống → font trong cache
của ứng dụng → Google Fonts → CTAN → fallback hiện có. Mỗi bước phải có
đúng family, style và glyph cần dùng. Font nhúng dạng subset không được xem
là một bản font đầy đủ có thể tải hoặc dùng cho mọi ký tự.

`GoogleFontsProvider` giữ luồng Google Fonts hiện có: tra catalogue Google
qua Fontsource, lấy stylesheet từ `fonts.googleapis.com`, rồi chỉ nhận file
font từ `fonts.gstatic.com`. `CtanFontProvider` dùng catalogue cục bộ đã kiểm
tra, không tìm kiếm tên font trên website bất kỳ. Chỉ kết quả `NotFound`
mới chuyển sang provider tiếp theo. Lỗi mạng CTAN được thử lại có giới hạn
trong cùng provider như mô tả bên dưới; metadata sai, font hỏng, thiếu style
hoặc hết nguồn tải hợp lệ kết thúc lượt tải và giữ draft/fallback hiện có.

`IFontProvider` cung cấp metadata/catalogue, các face, giấy phép, URL và
quy tắc redirect cùng danh sách mirror dự phòng tùy chọn. Thêm provider bằng implementation mới và đăng ký theo thứ
tự trong constructor của `FontDownloadService`; không cần viết lại luồng
mạng, hủy hay cài đặt. Provider có catalogue cục bộ trả metadata URL rỗng.
Mỗi provider phải phân biệt `NotFound` với dữ liệu `Invalid` và chỉ đánh dấu
`Found` cho nguồn/font có giấy phép đã được kiểm tra.

Luồng Aptos có sẵn vẫn dùng package chính thức của Microsoft; nó độc lập
với catalogue Google Fonts/CTAN và không mở rộng việc tìm kiếm nguồn tải.

## Font hệ thống trên Windows và macOS

`ApplicationFonts` dùng CoreText trên macOS và `WindowsSystemFonts` dùng
DirectWrite trên Windows để lấy đúng family và face thường/đậm/nghiêng/đậm
nghiêng đã cài. Windows từ chối face mô phỏng đậm/nghiêng và face gần nhất có
cờ style khác yêu cầu. Picker, bộ lọc ngôn ngữ và kiểm tra trước khi mở editor
dùng cùng face thật; coverage Windows được xác nhận bằng glyph index của
DirectWrite, không nhờ font fallback của Qt.

Nhánh font hệ thống chỉ nhận static TrueType có `glyf/loca`, tối đa 64 MiB
cho một face và quyền Installable hoặc Editable theo `OS/2.fsType`. CFF/CFF2,
variable, font màu/bitmap-only, Restricted và Preview & Print-only bị loại.
Thiếu style hoặc glyph tiếp tục dùng thứ tự resolve và luồng khôi phục font
hiện có; không tạo style giả hoặc đổi thứ tự provider.

Các bảng của face được chọn, kể cả face trong TTC, đi qua bộ dựng sfnt dùng
chung với macOS trong `ApplicationFonts`; PDFium nhận snapshot TrueType
đơn-face thay vì tự chọn face đầu tiên của collection. Save request giữ bytes
font, không giữ handle DirectWrite. Probe picker không dựng cả sfnt; lúc chuẩn
bị và ghi Save vẫn kiểm tra bytes và coverage đầy đủ theo nội dung cần ghi.

## Font cho biểu mẫu và Free Text khi Save

App không còn đóng gói Arial. `PDFSaveWriter::prepare()` chụp một face Unicode
TrueType khả dụng trên GUI thread cho Text Field, Dropdown, List Box và Free Text.
Thứ tự chọn là family mặc định của Qt, các family đã tải, Arial/Liberation Sans/
Noto Sans/DejaVu Sans, rồi các family hệ thống còn lại; với từng family, kiểm
tra cache trước hệ thống. Face phải qua kiểm tra quyền nhúng, có `glyf/loca`,
không quá 64 MiB và có ASCII cùng ký tự kiểm tra tiếng Việt U+1ECD.
Luồng này chỉ dùng font cục bộ, không tự tải font khi Save.

Worker nhận bytes và family layout đã đăng ký, không đọc đường dẫn Arial cũ.
Canonicalizer dựng tên PostScript, metrics, CID và ToUnicode từ chính snapshot;
`/AP` dùng cùng face. Resource `/TrePDFUF*` mới lưu SHA-256 của font. Chỉ tái sử
dụng resource khi digest và version khớp; font cũ không có digest được giữ cho
field chưa sửa, còn field đang sửa nhận resource phù hợp. Cache template chỉ
giữ một font trên mỗi worker và cập nhật khi snapshot đổi. Nếu không có face
phù hợp, Save báo lỗi và giữ bản nháp/tệp đích theo cơ chế lưu nguyên tử hiện có.

## Kiểm tra font trước khi chỉnh sửa

Khi xác thực font tải về, tên family legacy của Windows (OpenType name ID 1)
được đối chiếu với tên typographic (ID 16) trong cùng nhóm platform/encoding/
language của bảng `name`. Ví dụ `TeXGyreTermes` và `TeX Gyre Termes` là hai tên
được font khai báo cho cùng family. Chỉ nhận cặp có subfamily tương đương; không
bỏ khoảng trắng tùy ý hoặc gộp các face như Arial Narrow vào Arial. Cache và alias
layout giữ tên typographic để editor tìm lại đủ bốn style sau tải. Font bytes,
kiểm tra style, glyph và quyền nhúng giữ nguyên. Lỗi xác thực/cài cache ghi nguyên
nhân cụ thể trong log ứng dụng.

Khi người dùng mở editor cho một đoạn text, `requestSelectedTextEdit()` kiểm
tra mọi character span của đoạn, gồm family, kiểu đậm/nghiêng và các ký tự
hiện có. Các yêu cầu trùng family/style được gộp lại. Font nhúng nguồn chỉ
được xem là có sẵn khi đúng face và đọc được các ký tự của run; tên font
hoặc một font thay thế đang hiển thị được không đủ để vượt qua kiểm tra.
Font hệ thống, font đã tải và font bundled được kiểm tra theo đúng face.

Nếu còn thiếu font, editor chưa mở và popup liệt kê các family cần tải.
Chỉ sau khi người dùng đồng ý, ứng dụng mới tải tuần tự qua
`FontDownloadService`, dùng chung kho font và các kiểm tra của luồng Save.
Một lượt tải family lấy các kiểu thường, đậm, nghiêng và đậm nghiêng mà
provider cung cấp. Sau đó ứng dụng kiểm lại tất cả face/ký tự cần cho đoạn;
chỉ khi đủ mới tiếp tục mở editor. Bấm đậm/nghiêng/gạch chân để bắt đầu edit
cũng đi qua bước này và giữ thao tác định dạng cho caret cuối đoạn.

Yêu cầu chờ gắn với document, revision và đoạn đang chọn. Hủy, đổi đoạn,
đổi công cụ/tab, đóng tài liệu, thay đổi lịch sử hoặc bắt đầu Save hủy việc
mở editor đang chờ. Lỗi tải giữ nguyên nội dung và bản nháp; tải font không
tạo thao tác undo hoặc tự lưu PDF. Font đã được cài thành công vẫn có trong
kho riêng. Luồng khôi phục font lúc Save tiếp tục xử lý ký tự hoặc kiểu chữ
mới được thêm sau bước kiểm tra ban đầu.

## Cache và xác thực

`ApplicationFonts::downloadedFontDirectory()` lấy thư mục từ
`QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)` rồi
thêm `fonts`. Với organization `TreOffice` và application `TrePDF` được đặt
trong `src/main.cpp`, đường dẫn mặc định theo từng hệ điều hành là:

| Hệ điều hành | Thư mục font tải về |
| --- | --- |
| macOS | `~/Library/Application Support/TreOffice/TrePDF/fonts/` |
| Windows | `%LOCALAPPDATA%\TreOffice\TrePDF\fonts\` |
| Linux | `~/.local/share/TreOffice/TrePDF/fonts/` |

`~` là thư mục người dùng; `%LOCALAPPDATA%` thường là
`C:\Users\<user>\AppData\Local`. Trên Linux, nếu `XDG_DATA_HOME` là đường dẫn
tuyệt đối, thư mục sẽ là `$XDG_DATA_HOME/TreOffice/TrePDF/fonts/`; nếu biến
rỗng hoặc là đường dẫn tương đối thì dùng mặc định trong bảng.

Đường dẫn thực tế luôn do [QStandardPaths](https://doc.qt.io/qt-6/qstandardpaths.html)
trả về, có thể khác khi chạy trong sandbox hoặc chế độ kiểm thử; không hard-code
các đường dẫn trên trong ứng dụng. Nếu không xác định, tạo hoặc ghi được thư mục,
cài font báo lỗi, không chuyển sang thư mục khác.

Google Fonts, CTAN và Aptos dùng chung kho font riêng này cho từng người dùng.
Font được giữ trên đĩa qua các lần mở ứng dụng và chỉ đăng ký cho TrePDF,
không cài vào kho font hệ điều hành. ZIP Aptos chỉ là file tải tạm; các face
đã xác thực được lưu vào cùng kho này.

Tên file là SHA-256 của bytes font với đuôi `.ttf` hoặc `.otf` tương ứng. Ứng dụng đọc lại
cache khi khởi tạo registry, kiểm tra digest và cấu trúc trước khi đăng ký
bằng `QFontDatabase::addApplicationFont()` trên GUI thread. Mỗi face còn có tên
Qt riêng theo digest để worker Save tính layout. Qt có thể giữ font dự phòng
trong cache của worker nếu family công khai từng được hỏi trước khi download;
đăng ký font trên GUI thread không xóa cache đó. Tên riêng giúp vị trí từ khi
Save dùng đúng metrics của font vừa tải. Picker và metadata vẫn dùng family
công khai, còn PDF nhúng nguyên font program đã xác thực.

Font trong cache và font nhúng trùng bytes dùng chung một registration Qt
theo số chủ sở hữu. Reopen sau Save hoặc đóng một tab chỉ nhả quyền giữ font
của tài liệu đó. Không đăng ký rồi gỡ bản trùng: trên macOS việc này làm
worker dùng sai metrics ở lần Save tiếp theo dù tên family vẫn đúng.
Khi chủ sở hữu cuối cùng nhả font, lần đăng ký sau dùng tên riêng mới để tránh
tái sử dụng font engine cũ của worker. Digest font nguồn và family công khai
vẫn giữ nguyên; tên riêng này không được ghi vào PDF.

Font Type 1 nhúng dạng PFA cũng có tên hiển thị riêng khi Qt đọc được glyph
Unicode của run. `PDFFontAliases` chỉ đổi ba khai báo literal `FontName`,
`FamilyName`, `FullName` trong header ASCII trước `currentfile eexec`; giữ
nguyên encoding, metrics và phần mã hóa chứa outlines. Header giới hạn 64 KiB,
16.384 token, độ sâu 16; dạng PFB hoặc khai báo tên động/không hợp lệ dùng
fallback hiện có. Cách này giữ hình chữ monospace như Inconsolata khi vào
editor, thay vì dùng font hệ thống rồi bù khoảng cách trên hình chữ sai.
Khi Save dòng có nhiều font, writer có thể dùng lại từng handle Type 1 nguồn
khi fingerprint và style khớp, đồng thời mọi ký tự đã xuất hiện với đúng handle
đó trong các object nguồn. Không gộp coverage của hai font hoặc hai encoding
khác nhau; kiểm lại cả whitespace sau layout. Đây không phải nhánh tái nhúng
Type 1: ký tự chưa có mapping cần font đầy đủ hợp lệ hoặc Save báo thiếu font,
không âm thầm thay bằng Courier. Handle chỉ sống trên page của writer.

File tải từ CTAN được giữ nguyên bytes, không chuyển đổi outline trong cache.

Mỗi face phải đúng family/style, có glyph yêu cầu, font table hợp lệ và
quyền nhúng/chỉnh sửa cho phép theo `OS/2.fsType`. Service còn kiểm tra ASCII
và bộ ký tự tiếng Việt khi catalogue khai báo hỗ trợ. Chỉ chấp nhận static
TrueType hoặc OpenType/CFF; không nhận variable font, CFF2, font màu hay
font subset không đầy đủ. Các giới hạn byte, số file và thời gian vẫn áp dụng.

16 face TeX Gyre gốc có OS/2 version 3 và `fsType = 0x000c` sai quy tắc
mutually exclusive. `ReviewedCtanFonts` nhận riêng đúng bytes đã đối chiếu
GFL qua size/SHA-256, không nới kiểm tra chung hoặc sửa bytes font. Pagella
có metadata Installable hợp lệ nên không cần ngoại lệ. Xem
[manifest CTAN](ctan-font-manifest.md) về phiên bản, giấy phép và checksum.

Service bỏ qua các face đã có cache hợp lệ. Nếu face đang được yêu cầu đã có
trong cache và đủ glyph, lượt gọi hoàn tất trước khi gửi request mạng; nếu
thiếu face cần dùng, chỉ tải phần thiếu sau khi tra catalogue. Mọi face mới được xác thực trước khi
cài đặt cả batch bằng `QSaveFile`; lỗi hoặc hủy không cài một phần family.

## Catalogue CTAN

Danh mục nằm trong `src/assets/fonts/ctan-catalog.json`, được đóng gói thành
resource `:/trepdf/font-catalog/ctan-catalog.json` cho app và các test target.
`CtanFontProvider` đọc một lần, tra family và đường dẫn bằng index; không còn
danh sách family hay quy tắc ghép tên file TeX Gyre trong C++.

Snapshot ngày 2026-09-28 duyệt 3.725 file OTF/TTF trực tiếp trên CTAN, chọn
545 family / 1.123 face; 150 family đủ coverage tiếng Việt ở mọi face được
cung cấp. Toàn bộ 1.123 face đã qua validator thực tế với Qt 6.11 trên macOS.
Số liệu và lý do loại chi tiết nằm trong `docs/ctan-catalog-audit.json`.

Mỗi entry ghi đúng `name`, `license`, `source`, `subsets` và `faces`.
`faces` ánh xạ `regular`, `italic`, `bold`, `bolditalic` sang đường dẫn file
trên archive CTAN. Key không có nghĩa là style đó không được cung cấp; không
tạo nghiêng/đậm giả, không ánh xạ tên font thương mại sang font thay thế.
Ví dụ `Inconsolatazi4` có regular và bold, không có italic/bolditalic;
đây không phải alias của Google Fonts `Inconsolata`.

Parser giới hạn dung lượng, số family, độ dài chuỗi và đường dẫn; catalogue
hỏng hoặc trùng tên/path bị từ chối thay vì mở rộng nguồn tải. Thay đổi JSON
cần build lại ứng dụng, không lấy URL hoặc danh mục tùy ý từ PDF/người dùng.
Kiểm tra bytes, style, family, glyph và quyền nhúng lúc tải vẫn giữ nguyên.

Danh mục được tạo từ các OTF/TTF xuất bản trực tiếp trong chỉ mục CTAN,
không phải cam kết mọi font/gói TeX đều dùng được trong TrePDF. Font chỉ có
Metafont/Type 1, file chỉ nằm trong archive, variable/CFF2, font màu hoặc
face không thỏa điều kiện nhúng/nhận diện không tự động được đưa vào.
Các face có tên legacy/typographic và style khác nhóm cũng bị loại để tránh
backend Qt nhận nhầm family, chẳng hạn biến thể Light/Condensed được gộp
vào tên typographic chung. Bộ lọc này không sửa hay tự tạo alias cho font.

### Cập nhật danh mục

Chạy từ repository root bằng Python có `fontTools` (chỉ dùng lúc bảo trì,
không phải dependency runtime của app):

```sh
python3 scripts/fonts/update_ctan_catalog.py --cache-dir /tmp/trepdf-ctan-cache
python3 scripts/fonts/test_update_ctan_catalog.py
```

Script tải chỉ mục `FILES.byname.gz`, danh sách package/license từ CTAN JSON
API và bytes font vào cache bên ngoài repository. Nó đối chiếu đường dẫn
package, metadata giấy phép do CTAN khai báo, family/style trong sfnt, quyền
nhúng, outline tĩnh và đủ ASCII 32..126. Một face tối đa 16 MiB theo giới hạn
download service. Không tự mở rộng ngoại lệ `ReviewedCtanFonts`.

Kết quả gồm JSON dùng trong app và `docs/ctan-catalog-audit.json` ghi nguồn,
checksum, các file được chọn và lý do loại. Lỗi tải mặc định giữ catalogue cũ;
không âm thầm xuất bản danh mục thiếu do mạng. `--allow-partial` và `--package`
chỉ dùng khi điều tra, có đánh dấu `provenance.partial`; không dùng kết quả đó
để khẳng định đã quét toàn bộ. `--offline` tái tạo từ cache đã có; muốn lấy
snapshot mới từ upstream, dùng cache directory mới để không tái sử dụng bytes cũ.

Sau khi tạo, build lại app/test rồi chạy `loadsEveryCtanCatalogueEntry` và
`validatesExternalCtanCatalogFaces` với `TREPDF_TEST_CTAN_CACHE` trỏ tới
`<cache-dir>/files` (xem `tests/README.md`). Kiểm tra Qt thực tế là bắt buộc
trước khi nhận danh mục mới, vì metadata sfnt không đảm bảo mọi backend Qt
nhận cùng family/style. Không đưa thư mục cache tải font vào Git hay bundle app.

Nguồn chính thức:

- [Gói TeX Gyre trên CTAN](https://ctan.org/pkg/tex-gyre).
- [Thư mục OpenType](https://mirror.ctan.org/fonts/tex-gyre/opentype/).
- [GUST Font License](https://ctan.org/license/gfl).
- [LPPL 1.3c](https://www.latex-project.org/lppl/lppl-1-3c/).
- [Danh sách và quy tắc mirror CTAN](https://ctan.org/mirrors).
- [CTAN JSON API](https://ctan.org/help/json).

URL chính ghép `https://mirror.ctan.org` với đường dẫn chính xác từ JSON.
Các family có thể thuộc nhiều package, không bị giới hạn vào thư mục TeX Gyre.

URL ban đầu phải đúng endpoint và file trong catalogue. CTAN multiplexor có
thể trả HTTPS redirect sang mirror khác với prefix thư mục như `/CTAN`,
`/ctan` hoặc `/tex-archive`. Chỉ endpoint ban đầu được phép chọn host mirror;
những redirect sau phải giữ nguyên host. Mọi hop giữ chính xác suffix đường
dẫn của file được yêu cầu, không nhận query, credentials, fragment, port
riêng, địa chỉ IP trực tiếp hoặc tên localhost/local. Service phải kiểm tra
redirect thủ công từ URL đã được chấp nhận, giới hạn số hop và tiếp tục kiểm
tra HTTPS/TLS; không mở rộng kiểm tra URL ban đầu cho mọi mirror.

Nếu director hoặc mirror được chọn gặp lỗi kết nối, HTTP 403/404/408/429 hoặc
5xx, service thử tối đa hai nguồn dự phòng trong danh sách CTAN chính thức:
`https://ftp.fau.de/ctan/`, rồi `https://ftp.jaist.ac.jp/pub/CTAN/` (đối chiếu
[CTAN Sites](https://ctan.org/mirrors) ngày 2026-09-24). Mỗi nguồn chỉ được
tải đúng đường dẫn file trong catalogue; redirect từ các nguồn dự phòng
phải giữ nguyên host. Điều này xử lý trường hợp mirror do CTAN chọn trả
trang Cloudflare HTTP 403 thay cho file font. Mirror tải thành công tiếp tục
phục vụ các style còn lại trong lượt tải đó. Giới hạn 120 giây, hủy, kiểm tra
font và cài đặt nguyên tử vẫn áp dụng cho toàn bộ family; URL sai, redirect
không an toàn hoặc font không hợp lệ không được thử lại qua mirror khác.

### Format và lưu PDF

Catalogue có thể chứa static TrueType hoặc **OpenType với CFF outlines**.
Các face TeX Gyre và Inconsolatazi4 dùng OpenType/CFF, không phải TTF.
Không đổi đuôi hoặc giả định một font Qt hiển thị được thì writer sẽ nhúng đúng.
`FPDFText_LoadFont` chỉ công bố Type1/TrueType; implementation PDFium gán
`FontFile` hoặc `FontFile2` theo flag caller, không tự tạo `FontFile3` cho OTF.

`PDFSaveWriter` nhận diện `OTTO`, kiểm tra CFF tĩnh và nạp bằng nhánh Type1 của
PDFium. Sau khi PDFium serialize, `PDFOpenTypeFontWriter` dùng qpdf C API trên
file staging riêng để tìm đúng font program mới bằng SHA-256, chuyển
`/FontFile` thành `/FontFile3` với `/Subtype /OpenType`, và đặt phiên bản PDF
tối thiểu 1.6. Không sửa outline hay bytes font; giữ Identity-H, widths và
ToUnicode do PDFium tạo. Font không khớp digest không bị sửa. Việc đọc stream
và giải nén có giới hạn; không tìm đủ các font đã dùng thì Save thất bại.

PDFium mở lại kết quả và kiểm tra số trang trước bước ghi nguyên tử hoặc
áp dụng mật khẩu đang chờ. Lỗi ở bất kỳ bước nào giữ nguyên file đích và
draft. OTF tĩnh nhúng trong PDF được xác thực cùng cách và có thể dùng lại
ngay cả khi file cache đã bị xóa. Font mới nhúng giữ bảng shaping/kerning,
không bật subsetter của PDFium để tránh thay đổi advance ở lần sửa sau;
đổi lại PDF có thể lớn hơn. Nhánh CFF không mở rộng
probe font hệ thống để nhận mọi CFF/CFF2, variable hay font màu.

TrueType được nhúng với PostScript resource name riêng theo font program và
bảng mã CID; family/style công khai và các bảng glyph/layout được giữ nguyên.
Điều này tránh PDFium gộp hai font cùng tên nhưng khác bảng mã vào cùng resource.
Tên riêng dùng để đăng ký hiển thị PDF subset không được ghi vào metadata family
hoặc gửi tới provider.

### Giấy phép và provenance

Giấy phép và trang nguồn được ghi riêng cho từng family trong JSON; không
áp dụng GFL cho toàn bộ CTAN. Metadata package có thể liệt kê nhiều giấy phép
cho font và phần hỗ trợ TeX; khi phân phối lại phải giữ thông báo upstream
của những file thực sự được chọn.

Riêng TeX Gyre: GFL cho phép sử dụng, phân phối và sửa theo LPPL 1.3c hoặc mới hơn. GFL đề nghị
đổi tên font/file của bản dẫn xuất, nhưng nói rõ đề nghị đổi tên này không phải
yêu cầu pháp lý. Các nghĩa vụ LPPL về thông báo thay đổi và nguồn của bản dẫn
xuất vẫn áp dụng. Việc gộp font với một ứng dụng độc lập không đổi giấy phép
của ứng dụng.

Giữ bytes upstream nguyên bản. Provider ghi nguồn và tên giấy phép trong
catalogue (`IFontProvider::Family`); cache hiện lưu font theo digest, không
có sidecar metadata riêng.
Nếu phân phối lại một phần package hay font đã chuyển đổi, cần lưu thông báo
nguồn/thay đổi và cung cấp thông tin lấy bản đầy đủ chưa sửa. Không xem việc chỉ
tải vài file OTF là phân phối nguyên gói theo LPPL điều 2. Tài liệu upstream để
đối chiếu nằm dưới `https://mirror.ctan.org/fonts/tex-gyre/doc/`:

- `GUST-FONT-LICENSE.txt`.
- `README-TeX-Gyre-<Family>.txt` và `MANIFEST-TeX-Gyre-<Family>.txt`, với
  `<Family>` là `Termes`, `Heros`, `Pagella`, `Schola` hoặc `Cursor`.

Gói hoàn chỉnh có thể lấy tại
[tex-gyre.zip](https://mirror.ctan.org/fonts/tex-gyre.zip). Một số file giấy
phép nằm trong package vẫn mang phần mở đầu bản sơ bộ cũ; catalogue CTAN ghi
GFL 1.0, dựa trên cùng LPPL 1.3c-or-later. Khi phân phối lại, giữ nguyên file
thông báo upstream thay vì tự sửa văn bản giấy phép.

## Fixture kiểm thử

`tests/fixtures/fonts/tex-gyre/` chứa đủ bốn style Termes upstream nguyên bản,
v2.004; size và SHA-256 của từng file được ghi trong `NOTICE.md`.
Fixture giúp kiểm thử OpenType/CFF thật mà không phụ thuộc mạng hoặc font
cài trên máy. `NOTICE.md` cùng thư mục ghi nguồn, checksum, phạm vi chọn
file và cách lấy package đầy đủ; GFL, LPPL, README và manifest upstream
được giữ nguyên. Fixture chỉ phục vụ test, không thêm vào bundle của app.

`tests/assets/inconsolata/` chứa Regular/Bold nguyên bản của Inconsolatazi4
cùng thông báo nguồn và giấy phép upstream. Regression dùng network giả để
kiểm tra tải đúng hai style, cache, từ chối italic không có và Save/reopen
chuỗi URL có ký tự mới; không cần truy cập CTAN khi chạy suite thông thường.
