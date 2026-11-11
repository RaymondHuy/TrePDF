# Vẽ bảng

**Edit → Table** mở hộp chọn số hàng/cột, từ 1 đến 20 mỗi chiều, mặc định 3×3.
Nhấn **Draw table** rồi kéo trên trang để đặt bảng. Các ô chia đều theo khung
đã kéo. Đóng hộp chọn hoặc Escape trước khi xác nhận không đổi công cụ hiện tại.
Escape trong lúc kéo hủy preview, không tạo draft.

Bảng là một đối tượng: dùng Select để chọn, kéo di chuyển, kéo tám tay nắm
để resize, Delete để xóa. Khi bảng được chọn, kéo đường kẻ bên trong để đổi
độ rộng hai cột hoặc chiều cao hai hàng liền kề; khung ngoài và các đường kẻ
khác giữ nguyên. Các đường phân cách không thể vượt qua nhau. Escape khi
đang kéo khôi phục vị trí trước thao tác; mỗi lần kéo hoàn tất là một bước Undo.
Màu nét và độ trong suốt dùng panel thuộc tính Shape.
Undo/Redo hoạt động như các Shape khác. Bảng không có màu tô;
chưa hỗ trợ gộp ô.
Số hàng/cột chọn trong popup chỉ áp dụng cho bảng mới.

## Thêm, xóa hàng và cột

Chọn bảng để mở phần **Hàng và cột của bảng** trong panel thuộc tính bên phải.
Bấm **Thêm hàng** hoặc **Thêm cột** để mở popup chọn số lượng và vị trí
bắt đầu. Vị trí đánh số từ 1, chèn trước hàng/cột đang có tại số đó; chọn
vị trí bằng số hàng/cột hiện tại cộng 1 để thêm ở cuối. Mặc định thêm một
hàng/cột cuối bảng. Ví dụ bảng có 4 hàng, thêm 2 hàng tại vị trí 3 tạo hai
hàng trống mới ở vị trí 3–4; hai hàng cũ 3–4 chuyển thành 5–6.

Khi đang sửa nội dung ô, panel Thuộc tính ô chỉ cung cấp định dạng nội dung.
Hoàn tất hoặc hủy sửa ô để trở về panel thuộc tính bảng và sử dụng các nút
thêm/xóa hàng, cột.

Các ô cũ giữ nguyên kích thước vật lý, bảng mở rộng xuống dưới hoặc sang phải.
Hàng/cột mới lấy kích thước, định dạng mặc định và góc xoay từ hàng/cột tại
vị trí chèn; khi thêm cuối bảng thì lấy từ hàng/cột cuối. Ô mới trống, các ô
cũ giữ nội dung và định dạng theo vị trí mới. Nếu bảng vượt mép trang hoặc
giới hạn Header/Footer đang sửa,
thao tác báo lỗi để người dùng di chuyển hoặc thu nhỏ bảng trước. Mỗi chiều
vẫn tối đa 20; các nút tương ứng bị vô hiệu hóa khi đạt giới hạn.

Nội dung và cỡ chữ đang nhập phải hợp lệ trước khi chèn. Hoàn tất phần đang
nhập và chèn hàng/cột là hai bước Undo riêng; cả nhóm được thêm trong một
lần Áp dụng là một thay đổi nguyên tử, hỗ trợ Undo/Redo, Discard và Save/Save As.
Bảng mở lại tiếp tục
chèn và kéo đường phân cách được. Khung bảng nhận diện từ PDF cũng chèn được,
nhưng chữ/ảnh nguồn độc lập vẫn giữ nguyên vị trí như các thao tác khung khác.

**Xóa hàng** hoặc **Xóa cột** mở popup nhập các số thứ tự, ngăn cách bằng dấu
phẩy, ví dụ `1,3,5`. Các số được hiểu theo bảng trước khi xóa, không cần sắp
xếp; số trùng chỉ xóa một lần. Cho phép khoảng trắng, không nhận khoảng số,
số thập phân, mục trống hoặc số ngoài bảng. Mặc định điền số cuối cùng.
Áp dụng xóa toàn bộ nhóm trong một bước Undo, gồm nội dung và định dạng của
các ô đó. Khung bảng thu gọn từ dưới/phải, các hàng/cột còn lại dồn về
trên/trái và giữ kích thước, nội dung, định dạng.
Luôn giữ ít nhất một hàng và một cột; nút xóa tương ứng bị vô hiệu hóa khi
chỉ còn một. Muốn xóa cả bảng, chọn bảng rồi nhấn Delete.

Lỗi giữ popup và phần đã nhập để sửa, không thay đổi bảng hoặc lịch sử.
Hủy/Escape đóng popup mà không áp dụng. Đổi bảng/tài liệu, lưu hoặc thay đổi
lịch sử đóng popup để không áp dụng nhầm đối tượng. Undo khôi phục cả khung
lẫn nội dung ô bị xóa; Save/mở lại giữ đúng bảng đã thu gọn. Với khung nguồn chỉ
nhận diện đường kẻ, chữ/ảnh PDF độc lập vẫn giữ nguyên khi xóa hàng/cột.

## Khung bảng có sẵn trong PDF

TrePDF tự nhận diện khung lưới kín từ các nét thẳng hoặc dải chữ nhật mảnh
cùng màu, cùng độ dày. Bấm vào đường kẻ để chọn khung, kéo đường phân cách
để đổi độ rộng cột/chiều cao hàng; các tay nắm ngoài thay đổi kích thước khung.
Khung chỉ có một hàng dùng tay nắm trên/dưới để đổi chiều cao. Mỗi thao tác
hỗ trợ Undo/Redo, hủy kéo và Save/Save As.

Chỉ đường kẻ được gom nhóm. Chữ và ảnh giữ nguyên vị trí, tiếp tục chọn và
chỉnh sửa riêng; chúng không dồn dòng hoặc di chuyển theo khung. Xóa khung
giữ lại chữ và ảnh. Panel thuộc tính nhắc rõ hành vi này. Khung nhận diện
không mở trình nhập nội dung ô; bảng tạo bằng công cụ Table vẫn sửa ô như cũ.

Phạm vi nhận diện gồm lưới tối đa 20×20, ít nhất một đường phân cách, nét đặc
đục từ 0,75 đến 2,5 pt và toàn bộ đường phân cách nối kín hai mép. Các nét
phải ở cấp trang, không có marker/clip và nằm liên tiếp trong thứ tự vẽ.
Hỗ trợ dải đường kẻ bị chia thành nhiều đoạn, các miếng nối góc trùng nhau,
CropBox và góc xoay trang. Bảng scan, nét đứt, đường cong, ô gộp, viền thiếu,
độ dày/màu khác nhau và đường kẻ trong Form XObject chưa được gom nhóm.

## Sửa nội dung ô

Nhấp đúp vào một ô để sửa chữ **ngay trong ô**. Enter xuống dòng;
Ctrl/Cmd+Enter hoàn tất và Escape hủy phần đang nhập. Tab/Shift+Tab hoàn tất
ô hiện tại rồi chuyển sang ô kế tiếp/trước đó. Bấm ra ngoài, Save hoặc chuyển
công cụ/tab cũng hoàn tất nội dung hợp lệ trước khi tiếp tục. Nếu chữ không
vừa ô hoặc vượt giới hạn, thao tác bị chặn và phần đang nhập vẫn được giữ lại.
Mỗi lần hoàn tất là một bước Undo. Hoàn tất ô chỉ tạo draft; Save/Save As mới
ghi PDF.

Nội dung đang nhập thuộc workspace, độc lập với delegate trang được tái sử
dụng. Cuộn trang ra ngoài màn hình rồi quay lại không mất chữ; hủy hoặc thay
đổi lịch sử tài liệu loại bỏ phiên nhập cũ. Khung nhập đi theo zoom/xoay trang.

Panel Thuộc tính ô bên phải cung cấp font, cỡ chữ, Bold/Italic/Underline, thông báo
lỗi và Áp dụng/Hủy; nhập chữ vẫn diễn ra trực tiếp trong ô. Panel thay panel
thuộc tính hình trong lúc nhập và không che nội dung trang. Khi cuộn khỏi ô,
panel giữ nguyên; Chuyển đến ô đưa người dùng trở lại ô đang nhập. Định dạng áp
dụng cho đoạn đang bôi đen; khi không bôi đen, chỉ chữ gõ tiếp nhận định dạng
đã chọn. Ctrl/Cmd+B, I, U tương đương các nút trên panel. Font mặc định
dùng cùng cơ chế với Add Text; cỡ chữ mặc định 12 pt, cho phép chọn hoặc nhập
số nguyên từ 1 đến 512 pt, màu chữ đen. Cỡ chữ cũng áp dụng cho vùng chọn
hoặc chữ gõ tiếp; Save/Áp dụng nhận cả cỡ đang nhập trong panel. Các face phải
có sẵn và hỗ trợ ký tự, không tổng hợp Bold/Italic khi thiếu face. Giới hạn
1.024 đơn vị UTF-16 mỗi ô và 8.192 cho cả bảng; xuống dòng được chuẩn hóa về
LF, chữ về Unicode NFC. Mỗi ô tối đa 256 đoạn định dạng, cả bảng tối đa
1.024 đoạn; metadata vẫn có giới hạn kích thước. Nếu thiếu font, cài trong
Settings → Font. Định dạng của ô trống cũng được lưu để lần nhập sau dùng lại.

Chữ tự xuống dòng trong ô, cách đường kẻ 3 pt. Nội dung quá cao hoặc quá rộng
sẽ báo lỗi và giữ nguyên phần đang nhập. Kéo đường phân cách hoặc thu nhỏ bảng
chỉ áp dụng khi nội dung vẫn vừa tất cả các ô. Di chuyển bảng, xóa, Undo/Redo
và Discard luôn xử lý chữ cùng khung bảng.

**Xoay chữ** trong panel Thuộc tính ô chọn 0°, 90°, 180° hoặc 270° theo chiều
kim đồng hồ. Góc xoay áp dụng cho toàn bộ nội dung ô, độc lập với vùng bôi đen;
font/cỡ chữ/B/I/U tiếp tục áp dụng cho từng đoạn. Khung ô không xoay. Ở 90° và
270°, chiều rộng và chiều cao dùng để dàn chữ đổi chỗ, nên chữ xuống dòng và
được kiểm tra vừa ô theo hướng mới. Editor trực tiếp và preview cùng hướng
với chữ khi Save. Góc xoay được giữ khi sửa chữ, chuyển ô, cuộn khỏi trang,
Undo/Redo và lưu/mở lại, kể cả ô trống. Áp dụng hoặc Save hoàn tất cả nội dung
và góc xoay; Hủy/Escape bỏ phần thay đổi đang nhập.

## Dữ liệu và lưu PDF

`TableShape` được thêm cuối enum `ShapeType`, dùng `AddShapeTool` và pipeline
`addedShape`/`sourceShape` hiện có. Cấu hình `tableRows`/`tableColumns` thuộc
controller của tài liệu; mỗi candidate chụp riêng hai giá trị khi được tạo.
Draft giữ số hàng/cột hiện tại để chèn mà không sửa metadata nguồn của
candidate; Save vẫn đối chiếu fingerprint và cấu trúc nguồn trước khi ghi mới.
`tableRowStops`/`tableColumnStops` giữ vị trí đường phân cách trong khung vẽ
bên trong, với tỉ lệ từ 0 đến 1; vector rỗng tương đương chia đều. Candidate
giữ giá trị gốc, Draft giữ giá trị đã sửa. Move/resize khung giữ tỉ lệ này và
số hàng/cột. Hit-test nhận toàn bộ khung để người dùng kéo từ bên trong một ô;
các đường phân cách của bảng đang chọn ưu tiên thao tác đổi kích thước ô.

`ShapeDraftView` vẽ preview bằng Qt Quick Shapes; hai EditOverlay cho desktop
dùng cùng số hàng/cột từ candidate hoặc cấu hình placement. Tọa độ normalized
và phép biến đổi viewport tiếp tục xử lý zoom, xoay và CropBox theo pipeline
Shape. Công cụ Table được ẩn trong bản web chưa có backend hỗ trợ.

`PDFShapeUtils::createPath()` tạo một PDF path có đường viền ngoài và các
đường ngang/dọc bên trong, giữ nguyên dạng vector khi Save. Metadata
`TrePDFShape` version 2 dành cho Table chia đều chứa `rows`/`columns` cùng
geometry, paint và fingerprint đường vẽ. Bảng đã sửa đường phân cách dùng
version 3, thêm mảng `rowStops`/`columnStops` cho cả hai chiều. Reader tiếp tục
đọc version 2, nên bảng cũ cũng kéo được đường phân cách. Shape cũ tiếp tục
dùng version 1. Bảng có nội dung dùng version 4, bổ sung ID nhóm, nội dung ô,
font và cỡ chữ. Version 5 bổ sung các đoạn font/B/I/U theo chỉ số UTF-16
và định dạng mặc định từng ô. Version 6 lưu thêm cỡ chữ của từng đoạn;
reader tiếp tục đọc version 4 và 5 với cỡ 12 pt. Các text object con có marker
`TrePDFTableCell`; chúng là chữ
PDF thực để tìm kiếm/sao chép, không phải ảnh hoặc nét outline. Writer dùng
chung pipeline nhúng font với Add Text. Nét underline dùng child marker
của bảng, được kiểm tra geometry và liên kết với fragment chữ tương ứng;
di chuyển, xóa và suppression xử lý cả chữ lẫn các nét này.

Bảng có ô xoay dùng metadata version 8, lưu góc xoay của từng ô cùng các đoạn
định dạng. Child `TrePDFTableCell` version 4 lưu góc xoay và fingerprint của
chữ/nét gạch chân; reader kiểm tra hướng chữ, biên ô và cặp gạch chân trong
hệ tọa độ dàn chữ chưa xoay. Các bảng version 4–6 tiếp tục đọc ở góc 0°;
version 7 dành riêng cho khung nguồn vẫn giữ hành vi chỉ sửa đường kẻ.

`PDFTableFrameRecognition.h` quét riêng khung nguồn với giới hạn 50.000
đối tượng, 16.384 segment, 2.048 thành phần nét và 64 khung mỗi trang; có
kiểm tra hủy. Fingerprint gồm geometry, paint, thứ tự đối tượng và phép ánh
xạ trang; writer đối chiếu lại trước khi thay cả nhóm nét bằng một path.
Save đầu tiên sau khi chỉnh khung ghi metadata version 7 với `frameOnly`,
lưu đường phân cách và tiếp tục giữ chữ nguồn độc lập. Mở file hoặc chỉ chọn
khung không ghi metadata và không tạo draft.

Khi mở lại hoặc lưu sửa đổi, scanner/writer kiểm tra số hàng/cột là số nguyên
trong giới hạn, kiểm các mảng tỉ lệ đúng số lượng, hữu hạn, tăng dần và cách
nhau ít nhất 0,0001 (kể cả hai mép). Thao tác kéo thông thường giữ khoảng cách
0,005; bảng đã có ô hẹp hơn vẫn được điều chỉnh trong giới hạn hợp lệ.
Scanner kiểm cấu trúc marker, geometry, màu/opacity và so sánh path
thực tế với path chuẩn được dựng lại. Metadata tối đa 128 KiB, bảng 20×20 tối
đa 82 segment; shape thường giữ giới hạn riêng 32 segment. Marker hỏng không
được thử nhận diện như shape không marker. Khung nguồn không có marker đi
qua bộ nhận diện khung riêng với các điều kiện ở trên.

Scanner chỉ khôi phục bảng có chữ khi nhóm text con đầy đủ và hợp lệ; các
text con được loại khỏi danh sách Edit text độc lập. Writer kiểm tra lại nhóm
và fingerprint nguồn trước khi thay hoặc xóa để không để sót chữ cũ. Giới hạn
văn bản và metadata được kiểm tra cả khi đọc PDF, không chỉ lúc nhập.
Nếu PDFium thêm khoảng trắng phân cách giữa các text object khi trích xuất,
reader đối chiếu lại ký tự thực thuộc từng object, bỏ qua ký tự do PDFium tự
sinh. Khoảng trắng thực trong ô vẫn được giữ và kiểm tra; bước đối chiếu dự
phòng dùng index chung với giới hạn 50.000 ký tự mỗi trang.
Nhóm phải liên tiếp theo thứ tự khung rồi chữ; có đối tượng khác xen vào sẽ
không được chỉnh như bảng để tránh đổi thứ tự lớp vẽ. Index dùng chung trên
mỗi trang tránh quét lại toàn trang cho từng bảng, có kiểm tra hủy và giới hạn
50.000 đối tượng, 16 MiB metadata, 128 font/256 MiB dữ liệu font cần băm.

Các thay đổi chỉ tồn tại trong draft cho tới Save/Save As thành công. Hủy
placement không tác động PDF; lỗi lưu giữ bản nháp và file đích. Bảng đã lưu
có thể chọn, sửa nét, di chuyển, resize, kéo đường phân cách hoặc xóa sau khi
mở lại; tỉ lệ hàng/cột và nội dung từng ô được giữ nguyên.

## Kiểm thử

Regression trong `tests/tst_PDFEditControllerShapes.cpp` kiểm bảng mới,
giới hạn số hàng/cột, Undo/Redo, hit-test, move/resize, round-trip với PDF đã
xoay/CropBox, grid lớn, marker sai và thao tác qua popup/overlay. Kiểm thử ô
bao gồm chuẩn hóa Unicode, giới hạn/overflow, lịch sử nguyên tử và chữ PDF
thực sau Save, mở lại, sửa và xóa bảng.
Kiểm thử chèn hàng/cột bao gồm vị trí đầu/giữa/cuối, kích thước ô không đều,
dịch chuyển nội dung/định dạng/góc xoay, ô trống kế thừa style, giới hạn trang,
Undo/Redo, Discard, khung nguồn, lưu/mở lại và thao tác từ panel thuộc tính bảng.
Popup kiểm số lượng/vị trí thêm, danh sách xóa, hủy, lỗi và đổi đối tượng;
panel Thuộc tính ô không có nút thêm/xóa hàng, cột.
Kiểm thử xóa kiểm tra kích thước ô không đều, nội dung/định dạng còn lại,
Undo/Redo, giới hạn 1×1, lưu/mở lại và chữ nguồn độc lập.

Chạy suite Shape sau khi build `trepdf_edit_controller_tests`; build app và
`trepdf_qmllint` để kiểm tra tích hợp QML. Kiểm tra trực quan popup và preview
ở ngôn ngữ tiếng Việt, sau đó Save và mở lại để xác nhận các đường kẻ.
