# Kế hoạch tối ưu bộ nhớ TrePDF

Ngày rà: 2026-10-05. Trạng thái: đề xuất dựa trên source và phép đo hiện có;
chưa triển khai các tối ưu trong tài liệu này.

Số đo gốc và giới hạn của phép so sánh Stirling nằm trong
[báo cáo memory audit](memory-audit-2026-10-05.md).

## Thứ tự triển khai

| Thứ tự | Hạng mục | Mục tiêu chính | Mức bằng chứng hiện tại |
|---|---|---|---|
| 1 | R1 — Chọn tile theo vùng nhìn | Giảm raster lớn khi zoom | Cơ chế và workload đã xác nhận; chưa có A/B |
| 2 | Q1 — Tạo inline editor theo nhu cầu | Giảm object/editor tồn tại khi đọc, sau edit | Xác nhận tạo sẵn trong QML; chưa đo bytes |
| 3 | R2 — Tạo preview PDFium document khi cần | Giảm handle/cache phục vụ đọc thường | Xác nhận hai worker docs; cần chứng minh fast path an toàn |
| 4 | R3 — Release candidate document khi đóng session | Thu hồi sớm tài nguyên sau đóng file | Xác nhận cache thread-local và expiry 30 giây |
| 5 | Q2 — Chỉ tạo đúng loại draft view | Giảm object/binding khi có nhiều draft | Xác nhận cả ba nhánh được tạo |
| 6 | Q3 — Cập nhật draft model theo phần thay đổi | Giảm cấp phát tạm, tạo lại delegate, CPU/GC | Xác nhận đường dựng lại dữ liệu; cần profiler định lượng |
| 7 | R4 — Tối ưu preview ảnh được thêm vào PDF | Giảm backing store khi edit ảnh lớn | Cần workload Add Image; ca đã đo không có draft ảnh |
| 8 | R5 — Candidate cache theo bytes | Giới hạn RAM trên trang phức tạp | Ca đã đo chỉ khoảng 0,5 MiB; ưu tiên thấp |
| 9 | Q4 — Gom hình học search/highlight | Giảm số QML item trên trang nhiều kết quả | Chỉ nên làm nếu profiler xác nhận nghẽn |

Thứ tự thể hiện hướng thử nghiệm, không cam kết mức MB tiết kiệm. Mỗi thay đổi
cần được đo riêng để biết nguồn nào thực sự làm giảm process footprint.

## C++ và rendering

### R1 — Chọn tile trước khi full-page quá lãng phí

- Vị trí: `src/PDFRasterPlan.h:161`, `src/PDFPageItem.cpp:1813`.
- Hiện tại: ngưỡng full-page dựa trên toàn trang (32 MiB/4096 pixel). Một
  trang zoom lớn nhưng chỉ nhìn thấy một phần vẫn có thể render toàn trang.
- Hướng sửa: so chi phí full-page với preview + các tile nhìn thấy/preload;
  chọn tiled sớm khi lợi ích đủ lớn, có hysteresis để tránh chuyển qua lại.
- Giữ nguyên DPR, chất lượng chữ, gutter, fallback và suppression theo revision.
- Đo cùng trang, cùng cửa sổ, cùng zoom 400%, cùng sidebar/split. So sánh 400%
  với 500% không phải A/B hợp lệ. RasterBudget giảm không đồng nghĩa footprint
  giảm tương ứng; cần xem cả peak lúc đổi zoom và RAM sau idle.

### R2 — Giảm document preview mở sẵn

- Vị trí: `src/PDFFormWorker.cpp:945`, `:1105`, `:2581`.
- Hiện tại: worker giữ document/form environment và mở thêm preview document
  trên đường render; GUI còn một document riêng. Telemetry đã thấy hai worker
  docs ngay khi chỉ đọc PDF.
- Hướng sửa: nghiên cứu đường render chỉ đọc không cần preview clone; chỉ tạo
  preview khi callback cần mutation/suppression. Phải kiểm tra toàn bộ callback,
  form appearance, annotation và trạng thái runtime trước khi dùng fast path.
- Giữ nguyên ownership theo thread, process lock và giá trị form chưa Save.
  Không chia sẻ GUI document sang worker. Đây là thay đổi có rủi ro cao hơn R1/Q1.
- Đo PDF không form, có form, có annotation và có draft; handle count chỉ chứng
  minh lifecycle, không tự cho biết số bytes tiết kiệm.

### R3 — Thu hồi candidate document theo session

- Vị trí: `src/PDFEditControllerCandidates.cpp:188`, `:212`, `:1876`.
- Hiện tại: candidate scanner giữ document thread-local; hết hạn thread idle
  sau 30 giây hoặc đổi document/revision mới đóng handle đã cache.
- Hướng sửa: enqueue release trên chính worker khi session đóng, chỉ release
  cache có identity/generation tương ứng. Cleanup cũ không được đóng cache của
  một session/revision vừa mở lại.
- Đo ngay sau close, sau 5 giây và sau 35 giây; không kỳ vọng giảm nhiều RAM
  trong lúc vẫn đang edit. Hành vi hiện tại là cache có thời hạn, chưa phải leak.

### R4 — Preview ảnh lớn qua QQuickPaintedItem

- Vị trí: `src/DraftImageItem.h:8`, `src/DraftImageItem.cpp:45`.
- Hiện tại: `DraftImageView` dùng QQuickPaintedItem để vẽ QImage nguồn. Cần đo
  backing store/texture tại kích thước hiển thị, nhất là zoom và resize ảnh lớn.
- Nếu đây là nguồn đáng kể: thử preview có kích thước theo vùng hiển thị hoặc
  QSG texture với transform thay cho vẽ lại bằng QPainter. Giữ pixel gốc cho Save.
- QImage implicit sharing không tự đồng nghĩa mỗi reference sao chép pixel.
  Upload nguyên ảnh cực lớn lên GPU cũng có thể tốn hơn backing store hiện tại;
  phải chọn kích thước preview có giới hạn và quản lý texture trên render thread.
- Kiểm tra resize, rotate, undo/redo, split, Save/reopen và độ sắc nét trên Retina.

### R5 — Candidate cache có thêm ngân sách bytes

- Vị trí: `src/PDFEditControllerCandidates.cpp:1653`.
- Hiện tại: giới hạn thường 12 trang, có các trang được pin bởi selection/draft.
  Hai trang cùng số lượng có thể khác rất nhiều về text, glyph và ảnh.
- Hướng thử: byte LRU cho dữ liệu scan có thể tạo lại; tách nó khỏi tài nguyên
  bắt buộc của draft/history. Không xóa undo hoặc giảm chất lượng để đạt budget.
- Chỉ triển khai sau workload edit nhiều trang cho thấy cache đáng kể. Trong
  phép đo hiện có, candidates khoảng 0,5 MiB nên chưa giải thích RAM 400 MB.

## QML cần tối ưu ở đâu

### Q1 — Tách inline editor khỏi page delegate luôn tồn tại

- Vị trí: `src/Components/PdfPageDelegate.qml:173`,
  `src/Components/PageInteractionOverlay.qml:200`,
  `src/Components/EditOverlay.qml:4086`, `:4106`.
- Bằng chứng: PageInteractionOverlay tạo `PlatformComponents.EditOverlay {}`
  trực tiếp. Trong đó `TextEdit { id: editor }` được tạo sẵn; khung ngoài chỉ
  dùng `visible: root.editing`. `commitEditing()`/`cancelEditing()` kết thúc
  phiên edit nhưng không hủy TextEdit.
- Hướng sửa đầu tiên: đưa editor và phần UI chỉ dùng lúc nhập chữ vào Loader,
  tạo khi bắt đầu edit và hủy sau khi hoàn tất commit/cancel. Các hàm đang tham
  chiếu trực tiếp `editor` phải xử lý trạng thái Loader và callback đến trễ.
- Bước tiếp theo: tách lớp hiển thị draft khỏi lớp tương tác Edit, để chỉ tạo
  phần tương tác khi cần. Không unload toàn bộ overlay chỉ vì về tab View:
  draft chưa Save vẫn phải hiển thị, và link/form vẫn phải hoạt động.
- Không unload khi đang gõ, giữ local undo/redo, IME, focus, formatting và
  semantics commit-on-zoom/session change. Pooling không được commit nhầm trang.
- ListView đã virtualize: số editor hiện tại theo số delegate đã tạo/giữ trong
  pool, không phải một editor cho mỗi trang trong cả PDF.
- Đo số TextEdit/EditOverlay sống, MallocInUse và footprint khi Browse, mở/hủy
  editor nhiều trang, scroll, đổi tab, close/reopen. Bytes tiết kiệm chưa đo.

### Q2 — Không tạo cả text/image/shape cho mỗi content draft

- Vị trí: `src/Components/EditOverlay.qml:3205`, `:3236`, `:3249`.
- Bằng chứng: trong cùng draft delegate có Text, DraftImageView và ShapeDraftView;
  chọn loại hiển thị bằng `visible: !draftItem.isImage`,
  `visible: draftItem.isImage`, `visible: draftItem.isShape`.
- Hướng sửa: Loader chọn Component theo kind, chỉ tạo phần trình bày phù hợp;
  các selection/resize handles dùng chung nằm ngoài phần thay thế.
- Lợi ích xác định ở số object/binding. Không kết luận ba nhánh luôn có ba
  pixel buffer: item bị ẩn không nhất thiết có backing texture được cấp phát.
- Đo trên trang có nhiều loại draft, kiểm tra đổi selection/undo/redo và
  restoration overlay khi texture đổi revision.

### Q3 — Giảm dựng lại dữ liệu và delegate khi draft thay đổi

- Vị trí: `src/Components/EditOverlay.qml:293`, `:318`, `:367`, `:2327`;
  `src/PDFEditControllerDrafts.cpp:286`.
- Bằng chứng: `reloadData()` gọi `draftsForPage(page)`, tạo các map/list snapshot,
  dựng `displayedDrafts` và bảy danh sách theo loại rồi gán lại các Repeater model.
  Có guard theo page và pooled; không phải mọi thay đổi luôn reload mọi trang.
- Hướng sửa nếu profiler xác nhận: model ổn định keyed theo objectId, cập nhật
  đúng role/item đổi; hoặc cache snapshot theo page revision. Giữ phần dữ liệu
  restoration cần thiết cho suppression, không chỉ cache theo số lượng draft.
- Các list có thể chia sẻ dữ liệu nền; không coi mỗi lần gán là deep-copy ảnh.
  Chi phí cần đo là allocations, QVariant/JS conversion và delegate churn.
- Đây có thể cải thiện CPU/độ mượt rõ hơn RAM idle. Tránh thay toàn bộ model API
  chỉ vì muốn code ngắn hơn hoặc thấy nhiều dòng JavaScript.

### Q4 — Gom search/highlight thành geometry khi mật độ cao

- Vị trí: `src/Components/SearchHighlightLayer.qml:22`,
  `src/Components/EditOverlay.qml:2495`, `:2509`.
- Hiện tại: Repeater tạo item cho từng rectangle, kể cả phần của trang nằm
  ngoài viewport; clipping không làm giảm số object đã được tạo.
- Nếu tài liệu có nhiều match/markup mỗi trang: thử một QQuickItem vẽ geometry
  theo batch, hoặc cull phần trình bày theo viewport. Giữ đầy đủ model kết quả,
  điều hướng và trạng thái match hiện tại.
- Với ít kết quả, code hiện tại đơn giản và hợp lý; chưa có bằng chứng cần sửa.

## Những phần QML đã làm tốt

- `DocumentView.qml:682`: `reuseItems: true`, cacheBuffer giới hạn 256–640.
- `PdfPageDelegate.qml:140`: pooled item detach document/controllers.
- `PageThumbnailPanel.qml:104`: Loader tắt khi collapse; list tái sử dụng
  delegate, cacheBuffer 200; dùng chung raster budget/form worker.
- `Main.qml:125`: đổi session hủy workspace cũ và dựng lại theo session mới;
  không giữ mọi workspace của tất cả tab trong cây QML.
- `DocumentWorkspace.qml:653`, `:675`, `:833`: split và các panel theo nhu cầu;
  TextFormatPanel/CommentPanel còn unload nội dung khi collapse.
- Không thấy `Canvas`, `ShaderEffect`, `MultiEffect`, `layer.enabled` trong
  các file QML ở src khi rà. Không có cơ sở ưu tiên sửa hiệu ứng GPU ở đây.
- Giữ clip của viewport và chất lượng Retina. Không giảm cacheBuffer về 0
  hoặc tắt reuseItems mặc định để đổi RAM lấy giật khi cuộn.

## Cách chứng minh hiệu quả

1. Lấy baseline và bản vá trên cùng macOS/Qt/build configuration, cùng PDF,
   cửa sổ, DPR, zoom, trang, sidebar và split; lặp ít nhất ba lần từ process mới.
2. Đo cold start, Browse, Edit, gõ/undo/redo thật, zoom, scroll, nhiều tab,
   close tất cả session, idle và Quit. Có thêm fixture forms, ảnh lớn và nhiều
   annotations/search results cho các tối ưu có điều kiện.
3. Dùng phys_footprint làm KPI process, phân biệt peak ngắn với snapshot 5 giây.
   Báo MallocInUse, MallocReserved và RasterBudget/LeaseBytes thành nhóm riêng;
   không cộng/trừ các nhóm. Budget không phải process-resident bytes.
4. Dùng QML Profiler trong phiên chẩn đoán riêng để xem object creation,
   binding/JS hotspots; đo RAM cuối cùng trên bản chạy không bật profiler.
   Nếu tối ưu baseline sau close, dùng allocation/graphics trace ngắn để tìm
   owner; không thêm heap scan, forced GC hoặc allocator trimming vào telemetry.
5. Mỗi bản vá phải qua unit/integration tests liên quan và native Metal:
   render, rapid scroll/zoom, form/edit/save, pooling, split/rotation,
   close/reopen giữa render và normal Quit. Đo cả độ trễ để tránh giảm RAM
   nhưng làm trải nghiệm kém hơn.

## Phạm vi rà soát lần bổ sung này

Kế thừa lint và sáu nhóm rà C++ của memory audit; bổ sung đọc QML và đường
DraftImageItem/draftsForPage. Lint ba file C++ liên quan bổ sung có một cảnh
báo thứ tự include (`PDFEditControllerDrafts.cpp:10`), không phải nguyên nhân RAM.
Các hướng trên là cơ hội tối ưu, chưa phải kết luận leak hoặc benchmark bản vá.
Không thay đổi source thực thi, không chạy lại build/test cho thay đổi tài liệu.

Tài liệu Qt đối chiếu:

- [Loader.active và vòng đời object](https://doc.qt.io/qt-6/qml-qtquick-loader.html#active-prop).
- [Qt Quick: lazy initialization, lifetime và profiling](https://doc.qt.io/qt-6/qtquick-performance.html).
- [Repeater tạo toàn bộ delegate trong model](https://doc.qt.io/qt-6/qml-qtquick-repeater.html#considerations-when-using-repeater).
