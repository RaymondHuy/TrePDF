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

## Cache và xác thực

Cache nằm tại `QStandardPaths::AppLocalDataLocation/fonts`; tên file là
SHA-256 của bytes font với đuôi `.ttf` hoặc `.otf` tương ứng. Ứng dụng đọc lại
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

## CTAN / TeX Gyre

Catalogue cục bộ chỉ gồm TeX Gyre Termes, Heros, Pagella, Schola và Cursor,
mỗi family có bốn face regular, italic, bold và bold italic. Đây là năm family
thật; không ánh xạ tên font thương mại sang font thay thế. Upstream có glyph
tiếng Việt. Kiểm tra glyph thực tế vẫn thuộc bước xác thực font tải về.

Nguồn chính thức:

- [Gói TeX Gyre trên CTAN](https://ctan.org/pkg/tex-gyre).
- [Thư mục OpenType](https://mirror.ctan.org/fonts/tex-gyre/opentype/).
- [GUST Font License](https://ctan.org/license/gfl).
- [LPPL 1.3c](https://www.latex-project.org/lppl/lppl-1-3c/).
- [Danh sách và quy tắc mirror CTAN](https://ctan.org/mirrors).

Đường dẫn có dạng
`https://mirror.ctan.org/fonts/tex-gyre/opentype/texgyre<family>-<style>.otf`.
`<family>` chỉ nhận `termes`, `heros`, `pagella`, `schola`, `cursor`; `<style>`
chỉ nhận `regular`, `italic`, `bold`, `bolditalic`.

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

Các file upstream là **static OpenType với CFF outlines**, không phải TTF.
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

GFL cho phép sử dụng, phân phối và sửa theo LPPL 1.3c hoặc mới hơn. GFL đề nghị
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
