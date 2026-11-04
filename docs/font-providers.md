# Nguồn font và kho font người dùng

`IFontProvider` mô tả nguồn đã được kiểm tra; `FontDownloadService` chịu trách
nhiệm mạng, giới hạn dung lượng/thời gian/redirect, hủy, kiểm tra font và cài đặt
nguyên tử. Provider chỉ nhận tên font, không nhận nội dung hay đường dẫn PDF.

## Thứ tự và trách nhiệm

Thứ tự resolve là font nhúng trong PDF → font hệ thống → font trong cache
của ứng dụng → FontSource → Google Fonts → CTAN → fallback hiện có. Mỗi bước phải có
đúng family, style và glyph cần dùng. Font nhúng dạng subset không được xem
là một bản font đầy đủ có thể tải hoặc dùng cho mọi ký tự.

`FontSourceProvider` tra snapshot cục bộ của repository `tre-base/FontSource`
trước khi gửi bất kỳ request Google nào. Family không có trong snapshot đi tiếp
qua các provider cũ; family có trong snapshot tải trực tiếp từ commit đã ghim.
`GoogleFontsProvider` giữ luồng Google Fonts hiện có: tra catalogue Google
qua Fontsource, lấy stylesheet từ `fonts.googleapis.com`, rồi chỉ nhận file
font từ `fonts.gstatic.com`. `CtanFontProvider` dùng catalogue cục bộ đã kiểm
tra, không tìm kiếm tên font trên website bất kỳ. Chỉ kết quả `NotFound`
mới chuyển sang provider tiếp theo. Lỗi mạng CTAN được thử lại có giới hạn
trong cùng provider như mô tả bên dưới; metadata sai, font hỏng, thiếu style
hoặc hết nguồn tải hợp lệ kết thúc lượt tải và giữ draft/fallback hiện có.

`IFontProvider` cung cấp metadata/catalogue, các face, giấy phép, URL và
quy tắc redirect, kiểm tra integrity của bytes tải về và danh sách mirror dự phòng
tùy chọn. Thêm provider bằng implementation mới và đăng ký theo thứ
tự trong constructor của `FontDownloadService`; không cần viết lại luồng
mạng, hủy hay cài đặt. Provider có catalogue cục bộ trả metadata URL rỗng.
Mỗi provider phải phân biệt `NotFound` với dữ liệu `Invalid` và chỉ đánh dấu
`Found` cho nguồn/font có giấy phép đã được kiểm tra.

Luồng Aptos có sẵn vẫn dùng package chính thức của Microsoft; nó độc lập
với catalogue FontSource/Google Fonts/CTAN và không mở rộng việc tìm kiếm nguồn tải.

## Provider FontSource

Nguồn này là [tre-base/FontSource](https://github.com/tre-base/FontSource), khác với
dịch vụ Fontsource được Google provider dùng để tra metadata.
`src/assets/fonts/fontsource-catalog.json` được đóng gói ở
`:/trepdf/font-catalog/fontsource-catalog.json`. App chỉ đóng gói metadata,
không đóng gói các file font của repository.

Snapshot hiện tại ghim commit `8386afe71207bccf5b2d7bfbbb36697e4d0634a3`, gồm
Liberation Sans/Serif/Mono 2.1.5, Hack 3.003, Iosevka 34.9.1 và Sarasa Gothic CL
1.0.43; mỗi family có Regular, Italic, Bold và Bold Italic. CL là Classical
orthography; không ánh xạ tên `Sarasa Gothic` hoặc biến thể khác sang CL.
Hack không khai báo đủ tiếng Việt; ký tự thực tế cần dùng vẫn phải vượt qua
kiểm tra glyph của service và bước chuẩn bị edit/Save.

Provider chỉ cho phép HTTPS trên `raw.githubusercontent.com` với đường dẫn
`/tre-base/FontSource/<commit>/fonts/<family-key>/<version>/<file>` có trong
snapshot. Không nhận branch `main`, `latest`, query, credentials, port riêng,
fragment hoặc redirect. Không tải catalog/manifest từ mạng lúc chạy ứng dụng.
Parser từ chối snapshot hỏng, revision không hợp lệ, trùng tên/path và face
thiếu size/checksum; không tự mở rộng nguồn tải theo metadata từ PDF.

`IFontProvider::acceptsFaceData()` kiểm size và SHA-256 của từng face trước
khi `ApplicationFonts` đọc font. Sau đó vẫn kiểm family/style, outline tĩnh,
quyền nhúng, ASCII, tiếng Việt được khai báo và ký tự yêu cầu. Giữ giới hạn
download hiện tại **16 MiB/face**, deadline 120 giây cho cả family và cache dùng
chung 128 file/256 MiB. Cả 24 face trong snapshot đều dưới 16 MiB; giới hạn
64 MiB của font cục bộ không phải giới hạn tải qua provider.

Tên family chỉ được trim và so khớp không phân biệt hoa/thường; service vẫn
bỏ prefix subset PDF hợp lệ như `ABCDEF+`. Không tự sửa tên gõ sai
`Libreation Sans`, bỏ khoảng trắng giữa tên hoặc thay bằng Arial. Khi tải
`Liberation Sans`, service lấy face đang cần trước rồi các face còn thiếu.
Font cache hợp lệ tiếp tục được dùng theo chính sách cũ; checksum snapshot áp
dụng cho bytes mới tải, không ép thay các bản font người dùng đã cài hợp lệ.
Lỗi mạng/checksum/validation hoặc Cancel không cài một phần batch và giữ
nguyên draft. Lỗi của FontSource không âm thầm chuyển sang nguồn khác.

### Thêm font hoặc cập nhật snapshot

Sau khi thêm font, manifest và giấy phép vào FontSource rồi commit/push, chạy:

```sh
python3 scripts/fonts/update_fontsource_catalog.py \
  --repository ../FontSource \
  --revision <full-40-character-commit> \
  --output src/assets/fonts/fontsource-catalog.json
```

Script đọc nội dung tại commit bằng Git, đối chiếu manifest với bytes, style,
size/checksum và các file license/authors. File chưa commit không tham gia
snapshot; phiên bản cuối trong `versions` của mỗi family được chọn. Xem lại
diff JSON rồi build và chạy kiểm tra Qt thực tế:

```sh
TREPDF_TEST_FONTSOURCE_REPO=../FontSource QT_QPA_PLATFORM=offscreen \
  cmake-build-debug/tests/trepdf_edit_controller_tests validatesExternalFontSourceCatalogFaces
```

Kiểm tra commit/file đã public trước khi phát hành TrePDF. Thêm font vào
GitHub chưa tự thay đổi catalogue trong một bản app đã phát hành; cần tạo lại
snapshot và build. Cơ chế này giữ URL và checksum cùng một phiên bản được duyệt.

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

Nếu còn thiếu font, editor chưa mở và popup liệt kê các family cần tải cùng
tên font cục bộ có thể thay thế. Ba lựa chọn theo thứ tự là **Hủy**, **Dùng
font thay thế**, **OK**. Hủy giữ nguyên nội dung; OK đồng ý tải. Chỉ sau khi
người dùng đồng ý tải, ứng dụng mới tải tuần tự qua
`FontDownloadService`, dùng chung kho font và các kiểm tra của luồng Save.
Một lượt tải family lấy các kiểu thường, đậm, nghiêng và đậm nghiêng mà
provider cung cấp. Sau đó ứng dụng kiểm lại tất cả face/ký tự cần cho đoạn;
chỉ khi đủ mới tiếp tục mở editor. Bấm đậm/nghiêng/gạch chân để bắt đầu edit
cũng đi qua bước này và giữ thao tác định dạng cho caret cuối đoạn.

**Dùng font thay thế** mở editor mà không gửi yêu cầu tải font. Controller ưu
tiên family mặc định dùng chung với Header/Edit > Add Text
(`headerTextFontFamily()`), rồi tìm trong các family cục bộ khả dụng. Một family
chỉ được đề xuất khi đủ face đậm/nghiêng, ký tự và điều kiện nhúng cho toàn bộ
phần cần thay. Nếu không có family phù hợp, popup giải thích cần tải font và
vô hiệu hóa lựa chọn thay thế. Tên trong popup chính là family được áp dụng
cho editor và Save; không mặc định hứa dùng Arial khi máy chưa có Arial.

Chỉ các run/face thiếu được thay; font nguồn nhúng dùng được và font cục bộ
đủ coverage được giữ nguyên. Việc thay font thuộc cùng phiên chỉnh sửa với
nội dung mới: Escape khôi phục trạng thái trước khi mở editor, commit tạo
thao tác Undo/Redo bình thường và Save mới ghi vào PDF. Popup cho biết bố cục
có thể thay đổi do khác metrics của font. Lựa chọn này chỉ xuất hiện ở bước
chuẩn bị edit, không tự thay font trong hộp thoại khôi phục khi Save.

Yêu cầu chờ gắn với document, revision và đoạn đang chọn. Hủy, đổi đoạn,
đổi công cụ/tab, đóng tài liệu, thay đổi lịch sử hoặc bắt đầu Save hủy việc
mở editor đang chờ. Lỗi tải giữ nguyên nội dung và bản nháp; tải font không
tạo thao tác undo hoặc tự lưu PDF. Font đã được cài thành công vẫn có trong
kho riêng. Luồng khôi phục font lúc Save tiếp tục xử lý ký tự hoặc kiểu chữ
mới được thêm sau bước kiểm tra ban đầu.

Lỗi tải font khi mở editor hoặc Save hiển thị bằng popup **Không thể tải phông
chữ**, có nút OK để đóng (hoặc Escape), thay cho thông báo thoáng qua ở cuối
màn hình. Popup ghi tên font và đúng face gặp lỗi: **Thường (Regular)**,
**Đậm (Bold)**, **Nghiêng (Italic)** hoặc **Đậm nghiêng (Bold Italic)**.
Nếu metadata hợp lệ của nguồn tải không có face được yêu cầu, thông báo nêu
nguồn không cung cấp kiểu đó và liệt kê các kiểu hiện có. Ví dụ catalogue CTAN
của `Inconsolatazi4` có Regular/Bold, không có Italic/Bold Italic.
Không tìm thấy family, lỗi mạng, font sai/không dùng được và thiếu ký tự có
thông báo riêng; không suy diễn lỗi mạng thành font không hỗ trợ kiểu chữ.
Popup cũng nói rõ editor chưa mở/nội dung không đổi hoặc Save chưa thành
công/bản nháp vẫn còn. Đổi hoặc đóng tab/tài liệu xóa popup cũ.

## Thêm font từ thiết bị

**Thiết lập → Font** mở modal ngay cả khi chưa mở PDF. Người dùng nhập tên
family thực tế (ví dụ `Noto Sans`) và chọn file cho từng kiểu **Regular**,
**Bold**, **Italic**, **Bold Italic**. Có thể thêm từ một đến bốn kiểu; các
kiểu còn thiếu không được tạo giả. Tên là family trong metadata font, không
phải tên file hay bí danh tùy ý. Form cho phép đổi/xóa lựa chọn trước khi thêm;
hủy form không cài font.

Khung **Preview** cho phép gõ thử trước khi thêm. Hai nút **Bold** và **Italic**
chọn đúng file Regular/Bold/Italic/Bold Italic; bật cả hai dùng file Bold Italic.
Nếu chưa chọn hoặc chọn sai file cho kiểu đang xem, khung báo lỗi thay vì mô
phỏng kiểu chữ. Preview dùng family đọc từ file nên không cần nhập tên trước.
Font được đăng ký tạm bằng tên riêng, không ghi `user-font` hay thêm vào picker;
đổi file, đóng hoặc hủy modal sẽ nhả font preview. Nội dung gõ thử được giữ khi
đổi kiểu và không ghi vào PDF.

`UserFontManager` đọc một snapshot có giới hạn của mỗi file cục bộ, tối đa
64 MiB/file, rồi dùng validator chung của `ApplicationFonts`. Chỉ nhận static
TTF hoặc OTF/CFF đúng family/style, có quyền nhúng/chỉnh sửa PDF. Font variable,
collection TTC/OTC, CFF2, font màu và file lỗi bị từ chối. Thông báo sai
family/style ghi rõ kiểu yêu cầu và family/kiểu thực tế đọc từ file.

`ApplicationFonts::installUserFonts()` kiểm tra toàn bộ batch trước khi ghi,
dùng `QSaveFile` và rollback nếu ghi/đăng ký thất bại. Tên file lưu là SHA-256
của bytes gốc với đuôi `.ttf`/`.otf`; tên family nhập không được dùng làm
đường dẫn. Nhập lại cùng bytes không tạo bản thừa. Một phiên bản khác của
family/style đã có trong kho riêng bị từ chối, không âm thầm ghi đè.

Font tự thêm lưu riêng qua `ApplicationFonts::userFontDirectory()`, bằng
`AppLocalDataLocation/user-font`:

| Hệ điều hành | Thư mục font tự thêm |
| --- | --- |
| macOS | `~/Library/Application Support/TreOffice/TrePDF/user-font/` |
| Windows | `%LOCALAPPDATA%\TreOffice\TrePDF\user-font\` |
| Linux | `~/.local/share/TreOffice/TrePDF/user-font/` |

Linux dùng `$XDG_DATA_HOME/TreOffice/TrePDF/user-font/` khi `XDG_DATA_HOME`
là đường dẫn tuyệt đối. Đường dẫn thực tế luôn lấy từ `QStandardPaths`.

Khi khởi động, app kiểm tra và nạp `user-font` trước cache `fonts` vào cùng
registry. Hai thư mục chia sẻ giới hạn **128 file / 256 MiB**. Nếu font đã có
trong cache tải về, thao tác thêm vẫn lưu một bản độc lập vào `user-font`.
Font được đăng ký riêng cho TrePDF, không tải lên mạng hay cài vào hệ điều
hành. Font dùng được ngay trong picker, kiểm tra trước khi edit và Save, rồi
tiếp tục có sau khi mở app lại. Bộ lọc ngôn ngữ/kiểu chữ vẫn yêu cầu coverage
và đúng face; thêm font không tự đổi nội dung hay font của PDF đang mở.

## Cache tải về và xác thực

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
