# Memory telemetry (Debug)

`PDFMemoryTelemetry` trình bày ba nhóm độc lập: process memory, malloc heap,
và ước tính tài nguyên/budget của TrePDF. **Không cộng hoặc trừ các nhóm với
nhau.** `phys_footprint` là KPI chính cho memory thực của tiến trình; raster
budget không đại diện cho byte resident của tiến trình.

## Bật/tắt

CMake cache `TREPDF_ENABLE_MEMORY_TELEMETRY` mặc định `AUTO`: Debug bật,
Release/RelWithDebInfo/MinSizeRel tắt. Generator expression áp dụng riêng từng
configuration, kể cả Xcode/Ninja Multi-Config. `ON` cũng chỉ bật trong Debug;
`OFF` tắt cả trong Debug:

```sh
cmake -S . -B cmake-build-debug -DTREPDF_ENABLE_MEMORY_TELEMETRY=OFF
```

Target nhận compile definition `TREPDF_ENABLE_MEMORY_TELEMETRY=0` hoặc `1`.
Khi bằng 0, observer, registry, image ticket, worker/texture counter và log được
loại bỏ bằng preprocessor, kể cả include và lời gọi Mach/malloc statistics.
Không tạo timer. QML singleton vẫn tồn tại nhưng `enabled` là false, `snapshot`
rỗng và reset không làm gì. Raster budget vẫn duy trì accounting hiện có cùng
hai counter lease nhỏ dưới mutex sẵn có.

## Snapshot và logging

App sở hữu một timer GUI-thread, interval 5 giây, độc lập với session. Timer
vẫn log khi đã đóng tab cuối và `Sessions: 0`. Mỗi timeout ghi một khối
`[Memory]` chia ba nhóm, dùng MiB (1.048.576 byte). Không log allocation riêng
lẻ, đường dẫn tài liệu, nội dung text hay password.

Snapshot có đúng ba nhánh: `processMemory`, `mallocHeap`, `resourceEstimates`.
Mỗi metric có `{current, peak}`, đơn vị byte trừ các metric count. API debug
đã bỏ các key phẳng cũ, `trackedEstimatedBytes` và `unaccountedBytes`; không
còn tổng hay chênh lệch giữa process memory, heap và budget.

`PDFMemoryTelemetry` là QML singleton trong module `TrePDF`:

```qml
// Cho Debug Memory Panel tương lai; task này chưa thêm UI.
const memory = PDFMemoryTelemetry.snapshot
const footprint = memory.processMemory.phys_footprint.current
const heapInUse = memory.mallocHeap.mallocInUseBytes.current
const heapReserved = memory.mallocHeap.mallocReservedBytes.current
const rasterEstimate = memory.resourceEstimates.rasterBudget.leaseBytes.current
PDFMemoryTelemetry.resetPeaks()
```

`snapshotChanged` phát theo timer và khi reset trên singleton. Metadata
`estimatedOnly: true` chỉ nằm trong `resourceEstimates`, không áp dụng cho số
đo Mach/malloc. Mỗi nhóm process/heap có `available`; giá trị unavailable được
biểu diễn bằng QVariant invalid (QML undefined) và log `unavailable`, không
thay bằng 0 hoặc giữ current cũ như mẫu mới.

### Process memory

Nhánh `processMemory` có `primaryMetric: "phys_footprint"`. Trên macOS Debug,
mỗi snapshot hoặc peak reset đọc một lần `task_info(mach_task_self(),
TASK_VM_INFO, ...)`, kiểm tra kết quả và số field đủ `TASK_VM_INFO_REV1_COUNT`.

| Metric | Ý nghĩa |
|---|---|
| `phys_footprint` | KPI chính cho memory thực của tiến trình, được đánh dấu rõ trong log. |
| `resident_size` | Resident bytes theo Mach. |
| `virtual_size` | Virtual address-space bytes theo Mach; không diễn giải thành RAM thực. |

Các số liệu là góc nhìn khác nhau của cùng tiến trình, không cộng/trừ nhau.
Nếu Mach thất bại hoặc không phải macOS, `available` là false; peak đã có được
giữ tới reset. Tham chiếu
[Mach task_info.h của Apple](https://github.com/apple/darwin-xnu/blob/main/osfmk/mach/task_info.h).

### Malloc heap

Nhánh `mallocHeap` dùng `malloc_zone_statistics(nullptr, &stats)` để lấy tổng
**mọi malloc zone đã đăng ký**, không chỉ default zone. API chỉ đọc counters,
không enumerate allocation, trim hoặc gọi pressure relief. Lời gọi nằm ngoài
mutex telemetry. Tham chiếu
[malloc.h của Apple](https://github.com/apple-oss-distributions/libmalloc/blob/main/include/malloc/malloc.h).

| Metric | Nhãn log | Ý nghĩa |
|---|---|---|
| `mallocInUseBytes` | `MallocInUse` | Actual malloc heap usage: `size_in_use`. |
| `mallocReservedBytes` | `MallocReserved` | Allocator-reserved memory: `size_allocated`, bao gồm cả phần đang dùng và phần có thể giữ sau `free`. |

Không cộng in-use với reserved; không lấy footprint trừ heap để suy ra memory
chưa giải thích. Reserved không đồng nghĩa resident. Sau khi đóng hết PDF,
app/runtime và allocator vẫn có thể giữ heap; xem xu hướng riêng của từng KPI.
Nền tảng không phải macOS có `available: false` và current/peak invalid.

### TrePDF resource/budget estimates

Mọi metric bên dưới nằm trong nhánh `resourceEstimates`. Log có tiêu đề
`TrePDF resources - estimates and counts, not process-resident bytes`.

| Metric | Phạm vi |
|---|---|
| `rasterBudget.leaseBytes` | Nhãn `RasterBudget/LeaseBytes`, ghi rõ conservative CPU + GPU estimate. Đọc trực tiếp `PDFRasterBudget::Statistics::currentLeaseBytes/peakLeaseBytes`, không có bộ đếm raster thứ hai. Charge **2×** pixel buffer dự phòng CPU/texture overlap; cap vẫn 256 MiB. |
| `rasterBudget.leaseCount` | Nhãn `RasterBudget/LeaseCount`, số active lease đọc trực tiếp từ budget. |
| `pendingImageBytes` | QImage đang render, đợi GUI nhận hoặc upload; đo stride × height. Ticket dùng chung đi cùng result/entry, không giữ ảnh hoặc document. |
| `pendingImageUnleasedBytes`, `pendingImageRasterCoveredBytes` | Hai phần của pending image để chẩn đoán overlap. Ticket chỉ giữ weak reference tới lease global đủ bao phủ ảnh; ảnh sống lâu hơn lease chuyển sang unleased ở snapshot sau. Không cộng vào một tổng memory. |
| `textureCount`, `tileCount` | Scene-graph texture thực sự đang được node sở hữu; tile là tập con. Giảm sau khi node hủy texture. Không quy đổi count thành byte. |
| `visiblePageItemCount` | Item có document đã mở, window, effective visibility và vùng nhìn giao trang; gồm cả pane và thumbnail. |
| `candidatePageCount`, `candidateBytes` | Hợp page id trong candidate cache/comment index từng controller, page chỉ đếm một lần. Chỉ đo buffer đã materialize, không kích hoạt scan. |
| `draftCount`, `draftBytes`, `draftImageBytes` | Draft content, page operation, link, watermark; gồm image dialog watermark và inline-edit buffer active. Không gồm native AcroForm/font/library cache riêng. |
| `undoCommandCount`, `undoBytes`, `undoImageBytes` | Undo stack gồm before/after, page snapshots, links và watermarks. |
| `redoCommandCount`, `redoBytes`, `redoImageBytes` | Redo stack được báo riêng. |
| `searchResultCount`, `searchRectangleCount`, `searchBytes` | Kết quả trên GUI, rectangle, index theo trang và query; không gồm bộ nhớ tạm trên search worker. |
| `sessionCount` | Session đã mở document; loại session rỗng hoặc mở lỗi. |
| `workerDocumentCount` | Handle `PDFiumDocument` mở thành công ngoài GUI thread; move chỉ chuyển attribution, không đếm lại. Không gán byte cho PDFium. |

Raster lease tiếp tục tồn tại khi texture còn sống, kể cả sau khi CPU QImage
được bỏ. Do đó `RasterBudget/LeaseBytes` có thể vẫn lớn khi `PendingImg: 0`;
đây là budget estimate, không phải phép đo CPU/GPU resident allocation.

Thumbnail dùng `PDFPageItem`/`PDFRasterBudget` chung, không có cache ảnh độc lập;
`thumbnailSeparateCache: false` mô tả điều này. Các image subcategory nằm trong
category cha. Count và thumbnail alias không thêm byte; không tạo tổng chung
cho các diagnostic này.

## Peak, ước tính và thread

- Peak process, heap và hai phần pending coverage là mức lớn nhất **đã lấy
  mẫu**, không phải OS lifetime peak. Các counter tài nguyên gốc cập nhật theo
  acquire/release hoặc signal state có sẵn; không tạo timer scan container.
- Current giảm khi giải phóng. Peak chỉ rebase về current khi đóng document/
  session hoặc explicit debug reset; không reset khi pooling, clear search,
  Undo/Redo, Discard hoặc Save. Đóng session rebase sau khi child QObject dọn
  xong; tài nguyên còn sống ở tab/worker/scene graph khác vẫn được tính.
- QImage dùng `bytesPerLine() * height()`, có scanline padding. QVector dùng
  `capacity() * sizeof(element)`, QString dùng `capacity() * sizeof(QChar)`;
  hash/set dùng capacity × key/value size, không đoán overhead node.
- Observer chỉ đọc const buffer, không detach/copy pixel/mở PDFium. Trong mỗi
  edit controller, buffer implicit-sharing được quy về một category theo thứ
  tự cache → draft → undo → redo; ảnh chỉ còn trong history được chuyển về đó.
- Ước tính không bao phủ allocator overhead, QObject, Qt/driver staging, GPU
  allocation thực, PDFium decoder, QPainterPath internals, cache font riêng,
  native form/designer state hay tất cả snapshot tạm của save/worker.
- Process, heap và resource counters được lấy mẫu nối tiếp. Không giữ đồng thời
  mutex raster/telemetry, không lấy PDFium lock hay emit từ worker/SG thread.
  Observer controller chạy GUI-thread. Registry nhỏ sống tới cuối tiến trình
  trong Debug để TLS document/SG destructor báo release an toàn sau app teardown.
- Instrumentation không scan heap, thay allocator, cache policy, raster budget,
  render algorithm hoặc lifecycle PDFium.

## Kiểm thử

Bảy test `reportsMemoryTelemetry*` kiểm tra schema ba nhóm, không còn tổng/
chênh lệch cũ, nhãn KPI/estimate trong log, peak/reset, QImage shared lifetime,
lease coverage/expiry, cache/search/draft/history, session cleanup, render
cancellation, texture/tile cleanup và worker document moves/failures.

Test malloc tạo nondefault zone chỉ trong test để xác nhận tổng mọi zone,
allocate/free làm đổi heap nhưng không làm đổi resource estimates. Test log
đóng session cuối trước khi đợi snapshot tiếp theo. Render lifecycle chạy cả
offscreen và native Metal. `trepdf_memory_telemetry_disabled_tests` biên dịch
với flag 0 ngay trong Debug, kiểm tra API không có snapshot/timer.
