# Vẽ bảng

**Edit → Table** mở hộp chọn số hàng/cột, từ 1 đến 20 mỗi chiều, mặc định 3×3.
Nhấn **Draw table** rồi kéo trên trang để đặt bảng. Các ô chia đều theo khung
đã kéo. Đóng hộp chọn hoặc Escape trước khi xác nhận không đổi công cụ hiện tại.
Escape trong lúc kéo hủy preview, không tạo draft.

Bảng là một đối tượng: dùng Select để chọn, kéo di chuyển, kéo tám tay nắm
để resize, Delete để xóa. Màu nét và độ trong suốt dùng panel thuộc tính Shape.
Undo/Redo hoạt động như các Shape khác. Bảng không có màu tô hoặc nội dung ô;
chưa hỗ trợ gộp ô, thay đổi riêng từng hàng/cột hoặc nhận diện bảng có sẵn.
Số hàng/cột chọn trong popup chỉ áp dụng cho bảng mới.

## Dữ liệu và lưu PDF

`TableShape` được thêm cuối enum `ShapeType`, dùng `AddShapeTool` và pipeline
`addedShape`/`sourceShape` hiện có. Cấu hình `tableRows`/`tableColumns` thuộc
controller của tài liệu; mỗi candidate chụp riêng hai giá trị khi được tạo.
Move/resize chỉ đổi khung; không đổi số hàng/cột. Hit-test nhận toàn bộ khung
để người dùng có thể kéo từ bên trong một ô.

`ShapeDraftView` vẽ preview bằng Qt Quick Shapes; hai EditOverlay cho desktop
dùng cùng số hàng/cột từ candidate hoặc cấu hình placement. Tọa độ normalized
và phép biến đổi viewport tiếp tục xử lý zoom, xoay và CropBox theo pipeline
Shape. Công cụ Table được ẩn trong bản web chưa có backend hỗ trợ.

`PDFShapeUtils::createPath()` tạo một PDF path có đường viền ngoài và các
đường ngang/dọc bên trong, giữ nguyên dạng vector khi Save. Metadata
`TrePDFShape` version 2 dành cho Table chứa `rows`/`columns` cùng geometry,
paint và fingerprint đường vẽ. Shape cũ tiếp tục dùng version 1.

Khi mở lại hoặc lưu sửa đổi, scanner/writer kiểm tra số hàng/cột là số nguyên
trong giới hạn, kiểm cấu trúc marker, geometry, màu/opacity và so sánh path
thực tế với path chuẩn được dựng lại. Metadata tối đa 16 KiB, bảng 20×20 tối
đa 82 segment; shape thường giữ giới hạn riêng 32 segment. Marker hỏng không
được thử nhận diện như shape không marker. Những đường kẻ của bảng bên ngoài
TrePDF không tự gộp thành đối tượng Table.

Các thay đổi chỉ tồn tại trong draft cho tới Save/Save As thành công. Hủy
placement không tác động PDF; lỗi lưu giữ bản nháp và file đích. Bảng đã lưu
có thể chọn, sửa nét, di chuyển, resize hoặc xóa sau khi mở lại.

## Kiểm thử

Regression trong `tests/tst_PDFEditControllerShapes.cpp` kiểm bảng mới,
giới hạn số hàng/cột, Undo/Redo, hit-test, move/resize, round-trip với PDF đã
xoay/CropBox, grid lớn, marker sai và thao tác qua popup/overlay.

Chạy suite Shape sau khi build `trepdf_edit_controller_tests`; build app và
`trepdf_qmllint` để kiểm tra tích hợp QML. Kiểm tra trực quan popup và preview
ở ngôn ngữ tiếng Việt, sau đó Save và mở lại để xác nhận các đường kẻ.
