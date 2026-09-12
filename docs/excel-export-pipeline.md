# Xuất bảng PDF sang Excel

Tab **Chuyển đổi** chỉ có nút **Xuất Excel** và nút **Hủy** khi đang xuất.
Kết quả được thông báo qua thông báo trạng thái chung của ứng dụng. Mỗi bảng nhận diện được tạo một sheet `Trang N Bảng M` trong file `.xlsx`.
Lưu mọi thay đổi PDF trước khi xuất; nội dung được đọc từ PDF đã lưu.

## Phạm vi nhận diện

- Bảng có lớp văn bản, đường viền ngoài kín, ít nhất hai hàng và hai cột.
- Nhận đường thẳng có stroke, đường chia ô ghép từ nhiều đoạn, viền chữ nhật
  và đường kẻ dạng chữ nhật mỏng tô màu, kể cả nhiều hình chữ nhật nằm trong
  cùng một path. Các đoạn dừng ở mép đường kẻ vuông góc được nối theo độ dày
  thực của đường kẻ; khoảng hở ở nơi không có giao điểm vẫn bị từ chối.
  Các trục đường kẻ lệch nhau tối đa một PDF point được căn chung; dung sai
  nối đầu mút vẫn giữ riêng để không che khoảng hở thực.
- Giữ ô trống, chữ Unicode, nội dung nhiều dòng trong một ô, thứ tự hàng/cột.
- Giữ ô gộp theo hàng/cột khi vùng gộp là hình chữ nhật và không có đường
  chia dang dở bên trong. Chữ được ghi vào ô đầu của vùng gộp.
- Giữ dấu chú thích nhỏ ở cuối cùng dòng chữ; đổi hai ký hiệu tick/cross
  private-use đã biết của Wingdings thành Unicode ✓/☒ để hiển thị trong Excel.
- Mỗi giá trị được ghi dưới dạng text: mã `0012` giữ số 0 đầu, chuỗi `=1+2`
  vẫn là nội dung văn bản. Không suy đoán kiểu số/ngày hoặc công thức.
- PDFium ánh xạ CropBox và rotation nguồn cho cả chữ lẫn đường kẻ. Chữ trong
  bảng phải nằm ngang và đọc trái sang phải sau phép ánh xạ này.
- Không OCR, không nhận bảng không đường kẻ, không xuất ảnh, annotation hay
  AcroForm thành dữ liệu bảng. Chưa hỗ trợ vùng gộp không phải hình chữ nhật,
  bảng có đường viền ngoài thiếu hoặc đường chia ô dang dở,
  chữ xoay/nghiêng hoặc chữ vượt qua ranh giới ô.
- Không tìm thấy bảng thì không ghi file. Nếu gặp một lưới bảng không thể gán
  chữ chính xác hoặc vượt giới hạn, toàn bộ tác vụ thất bại; không xuất kết quả dở.

## Các lớp

1. `Main.qml` giữ session khi mở hộp thoại lưu, kiểm tra lại membership rồi gọi
   `PDFTabModel::requestExcelExport()`. Model chặn file đích trùng canonical path
   với PDF trong bất kỳ tab đang mở nào.
2. `PDFDocumentSession` sở hữu một `PDFExcelExportController`. Guard kết thúc
   editor/focus rồi kiểm tra draft và Save. History, Save, đổi nguồn/cấu trúc
   trang hoặc đóng session sẽ hủy tác vụ đang chạy.
3. `PDFExcelExportController` chụp đường dẫn, revision, danh sách trang, thông tin
   file nguồn và mẫu tên sheet trên GUI thread. Pool riêng chỉ chạy một export
   toàn ứng dụng, không xếp hàng vô hạn. Worker mở handle PDFium riêng; mỗi
   transaction đọc trang giữ `PDFiumProcessLock`. Callback tiến độ kiểm tra
   `QPointer`, request, đường dẫn, revision và cancellation.
4. `PDFTableExtractor` đọc đường kẻ và glyph trên một trang. `PDFTableLayout`
   nhận geometry thuần, hợp các đoạn cùng trục, gom các lưới liên thông, nối
   các ô cơ sở không có đường ngăn và xác nhận vùng chữ nhật có viền kín,
   rồi gán glyph vào ô/vùng gộp và sắp thứ tự văn bản.
5. `SpreadsheetWorkbook` là interface ghi file, dùng `SpreadsheetSheet`,
   `SpreadsheetRow`, `SpreadsheetCell`, `SpreadsheetMerge`. Merge dùng hình
   chữ nhật gồm `firstRow`, `firstColumn`, `lastRow`, `lastColumn` (tọa độ từ 0,
   bao gồm hai góc), lưu trong `SpreadsheetSheet::mergedRanges`. Nội dung ô
   gộp chỉ ở góc trên trái; các ô còn lại trống hoặc không có record.
   Đây là dữ liệu Qt thuần, không có
   PDFium handle hoặc kiểu dữ liệu libxlsxwriter.
6. `LibxlsxwriterWorkbook.cpp` triển khai interface bằng libxlsxwriter 1.2.4,
   dùng chế độ bộ nhớ thường vì libxlsxwriter không hỗ trợ ô gộp nhiều hàng
   trong `constant_memory`. Adapter chặn tổng số ô và byte text trước khi thêm
   sheet, kiểm tra tên sheet, tọa độ, nội dung, vùng gộp chồng lấn và text bị
   che bởi vùng gộp. Chỉ tệp này include header backend.

## Thay backend

Implement ba phương thức `open(stagingPath, error)`, `writeSheet(sheet, error)`,
`finish(error)` của `SpreadsheetWorkbook`; đổi `createSpreadsheetWorkbook()`
và dependency CMake. Không cần sửa QML, session, controller hay detector.

Adapter mới phải giữ thứ tự hàng, tọa độ cột, Unicode, ô trống và vùng gộp; ghi chuỗi
đúng nghĩa text, báo lỗi thay vì tự cắt dữ liệu. Destructor trước `finish()`
phải giải phóng toàn bộ tài nguyên tạm. Adapter chỉ ghi đường dẫn staging mà
caller cấp; controller chịu trách nhiệm thay thế file đích.

Nguồn libxlsxwriter tối thiểu được vendor để build offline và link static.
Xem `third_party/libxlsxwriter/README.trepdf.md` cho provenance, checksum và
patch đóng row-spill file khi hủy/lỗi packaging. Không phụ thuộc Homebrew.

## Giao dịch và giới hạn

Workbook được hoàn tất trong `QTemporaryDir`, sau đó copy từng khối 256 KiB vào
`QSaveFile` với direct-write fallback tắt. Cancellation và commit dùng chung
mutex; hủy trước commit giữ nguyên file đích. Trước commit kiểm tra lại kích
thước và thời gian sửa file nguồn. Hủy sau khi commit hoàn tất vẫn được báo là
thành công vì file đã được ghi. GUI không giữ dữ liệu workbook của toàn tài liệu.

Một trang giới hạn 20.000 object, độ sâu Form 8, 40.000 path segment,
4.096 đường kẻ, 100.000 ký tự, 100.000 ô và 128 bảng. Phép tìm giao đường kẻ
giới hạn 2 triệu cặp; gán glyph giới hạn 13 triệu lượt. Một lần xuất tối đa
1.000 bảng; một ô tối đa 32.767 UTF-16 code unit theo contract workbook.
Backend giới hạn toàn workbook ở 250.000 ô được cấp phát và 32 MiB text UTF-8;
mỗi sheet tối đa 16.384 vùng gộp. Ngân sách ô bao gồm các ô trống trong vùng gộp,
chỉ đếm một lần với anchor/record trống đã có. Vượt giới hạn sẽ hủy toàn bộ
file staging và giữ nguyên file đích.

## Kiểm thử

```sh
cmake --build cmake-build-debug --target trepdf trepdf_qmllint trepdf_excel_export_tests
QT_QPA_PLATFORM=offscreen cmake-build-debug/tests/trepdf_excel_export_tests
cmake --build cmake-build-debug --target trepdf_spreadsheet_workbook_tests
cmake-build-debug/tests/trepdf_spreadsheet_workbook_tests
QT_QPA_PLATFORM=offscreen cmake-build-debug/tests/trepdf_edit_controller_tests exposesExcelExportTabAndDraftGuard
```

Suite Excel kiểm tra geometry, nhiều bảng, ô trống, nhiều dòng, Unicode,
merged/ambiguous grids, giới hạn, cancellation, PDF thật có đường stroke và
filled rectangles (riêng lẻ/compound, nối tại mép viền), XLSX/XML, literal text, cleanup file descriptor, bảo toàn
file đích khi lỗi/hủy, guard, tác vụ đồng thời và controller destruction.
Test tích hợp QML kiểm tra tab Chuyển đổi, BrowseTool và khóa xuất khi có draft.
Suite workbook kiểm tra vùng gộp ngang/dọc, từ chối overlap hoặc dữ liệu bị
che dưới ô gộp, giới hạn bộ nhớ và giải phóng tài nguyên khi hủy/lỗi.

Kiểm tra macOS thủ công: mở PDF có bảng, chọn Chuyển đổi → Xuất Excel, xác nhận
đường dẫn `.xlsx`, kiểm tra thông báo thành công và nội dung workbook. Thử
Cancel và không có bảng, rồi thoát bình thường.
