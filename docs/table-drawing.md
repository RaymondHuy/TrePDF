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
chưa hỗ trợ gộp ô hoặc nhận diện bảng có sẵn.
Số hàng/cột chọn trong popup chỉ áp dụng cho bảng mới.

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

Phiên bản này dùng một font cho cả bảng, chọn cùng cơ chế font mặc định của
Add Text, cỡ 12 pt, màu đen. Giới hạn 1.024 đơn vị UTF-16 mỗi ô và 8.192 cho
cả bảng; xuống dòng được chuẩn hóa về LF, chữ về Unicode NFC. Không hỗ trợ
rich text hoặc đổi font riêng từng ô. Nếu font không có đủ ký tự, người dùng
cần cài font phù hợp trong Settings → Font.

Chữ tự xuống dòng trong ô, cách đường kẻ 3 pt. Nội dung quá cao hoặc quá rộng
sẽ báo lỗi và giữ nguyên phần đang nhập. Kéo đường phân cách hoặc thu nhỏ bảng
chỉ áp dụng khi nội dung vẫn vừa tất cả các ô. Di chuyển bảng, xóa, Undo/Redo
và Discard luôn xử lý chữ cùng khung bảng.

## Dữ liệu và lưu PDF

`TableShape` được thêm cuối enum `ShapeType`, dùng `AddShapeTool` và pipeline
`addedShape`/`sourceShape` hiện có. Cấu hình `tableRows`/`tableColumns` thuộc
controller của tài liệu; mỗi candidate chụp riêng hai giá trị khi được tạo.
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
font và cỡ chữ. Các text object con có marker `TrePDFTableCell`; chúng là chữ
PDF thực để tìm kiếm/sao chép, không phải ảnh hoặc nét outline. Writer dùng
chung pipeline nhúng font với Add Text.

Khi mở lại hoặc lưu sửa đổi, scanner/writer kiểm tra số hàng/cột là số nguyên
trong giới hạn, kiểm các mảng tỉ lệ đúng số lượng, hữu hạn, tăng dần và cách
nhau ít nhất 0,0001 (kể cả hai mép). Thao tác kéo thông thường giữ khoảng cách
0,005; bảng đã có ô hẹp hơn vẫn được điều chỉnh trong giới hạn hợp lệ.
Scanner kiểm cấu trúc marker, geometry, màu/opacity và so sánh path
thực tế với path chuẩn được dựng lại. Metadata tối đa 128 KiB, bảng 20×20 tối
đa 82 segment; shape thường giữ giới hạn riêng 32 segment. Marker hỏng không
được thử nhận diện như shape không marker. Những đường kẻ của bảng bên ngoài
TrePDF không tự gộp thành đối tượng Table.

Scanner chỉ khôi phục bảng có chữ khi nhóm text con đầy đủ và hợp lệ; các
text con được loại khỏi danh sách Edit text độc lập. Writer kiểm tra lại nhóm
và fingerprint nguồn trước khi thay hoặc xóa để không để sót chữ cũ. Giới hạn
văn bản và metadata được kiểm tra cả khi đọc PDF, không chỉ lúc nhập.
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

Chạy suite Shape sau khi build `trepdf_edit_controller_tests`; build app và
`trepdf_qmllint` để kiểm tra tích hợp QML. Kiểm tra trực quan popup và preview
ở ngôn ngữ tiếng Việt, sau đó Save và mở lại để xác nhận các đường kẻ.
