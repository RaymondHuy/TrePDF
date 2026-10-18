/* PDFium lives in its own WebAssembly heap, independent of Qt's toolchain.
 * Keep this classic worker beside the pinned pdfium.js and pdfium.wasm.
 * PDF contents never leave this worker's origin or execute JavaScript.
 */
"use strict";

const MAX_FILE_BYTES = 64 * 1024 * 1024;
const MAX_PAGES = 2000;
const MAX_DIMENSION = 4096;
const MAX_PIXELS = 4_000_000;
let currentDocument = 0;
let currentBytes = 0;
let currentGeneration = -1;
let newestGeneration = -1;
let pageSizes = [];
let editor = null;

importScripts("./pdfium-source.js");
importScripts("./pdfium-editor.js");

const ready = new Promise((resolve, reject) => {
    self.Module = {
        locateFile: name => new URL(name, self.location.href).href,
        print: () => {},
        printErr: () => {},
        onAbort: () => reject(new Error("The PDF engine could not start. Reload this page.")),
        onRuntimeInitialized: () => {
            try {
                self.Module._FPDF_InitLibrary();
                resolve(self.Module);
                self.postMessage({ type: "ready" });
            } catch (_) {
                reject(new Error("The PDF engine could not start. Reload this page."));
            }
        }
    };
    try {
        importScripts("./pdfium.js");
    } catch (_) {
        reject(new Error("The PDF engine could not load. Reload this page."));
    }
});
// Also report startup failure when no PDF has been selected yet.
ready.catch(error => self.postMessage({ type: "error", generation: -1, message: error.message }));

function closeDocument(engine) {
    if (currentDocument) engine._FPDF_CloseDocument(currentDocument);
    if (currentBytes) engine._free(currentBytes);
    currentDocument = 0;
    currentBytes = 0;
    currentGeneration = -1;
    pageSizes = [];
    editor = null;
}

function openDocument(engine, request) {
    if (!(request.bytes instanceof ArrayBuffer) || request.bytes.byteLength === 0)
        throw new Error("Select a nonempty PDF file.");
    if (request.bytes.byteLength > MAX_FILE_BYTES)
        throw new Error("The web preview supports PDF files up to 64 MiB.");
    closeDocument(engine);
    let sizePointer = 0;
    try {
        currentBytes = engine._malloc(request.bytes.byteLength);
        if (!currentBytes) throw new Error("There is not enough browser memory to open this PDF.");
        engine.HEAPU8.set(new Uint8Array(request.bytes), currentBytes);
        currentDocument = engine._FPDF_LoadMemDocument(currentBytes, request.bytes.byteLength, 0);
        if (!currentDocument) {
            const code = engine._FPDF_GetLastError();
            throw new Error(code === 4
                ? "Password-protected PDFs are not supported in the web preview yet."
                : "This file could not be opened as a PDF.");
        }
        const count = engine._FPDF_GetPageCount(currentDocument);
        if (count < 1 || count > MAX_PAGES)
            throw new Error("The web preview supports PDFs with 1 to 2,000 pages.");
        sizePointer = engine._malloc(8);
        if (!sizePointer) throw new Error("There is not enough browser memory to read this PDF.");
        for (let page = 0; page < count; ++page) {
            if (!engine._FPDF_GetPageSizeByIndexF(currentDocument, page, sizePointer))
                throw new Error("The PDF contains a page that could not be read.");
            const width = engine.HEAPF32[sizePointer / 4];
            const height = engine.HEAPF32[sizePointer / 4 + 1];
            if (!Number.isFinite(width) || !Number.isFinite(height)
                || width <= 0 || height <= 0 || width > 1_000_000 || height > 1_000_000)
                throw new Error("The PDF contains unsupported page dimensions.");
            pageSizes.push({ width, height });
        }
        currentGeneration = request.generation;
        editor = new TrePdfEditor(engine, request.bytes, currentDocument, pageSizes);
        self.postMessage({ type: "opened", generation: currentGeneration, pages: pageSizes, ...editor.state() });
    } catch (error) {
        closeDocument(engine);
        throw error;
    } finally {
        if (sizePointer) engine._free(sizePointer);
    }
}

function renderPage(engine, request) {
    if (!currentDocument || request.generation !== currentGeneration)
        throw new Error("Open a PDF before requesting a page.");
    if (!Number.isSafeInteger(request.page) || request.page < 0 || request.page >= pageSizes.length)
        throw new Error("This page is outside the current PDF.");
    if (!Number.isFinite(request.width) || request.width <= 0)
        throw new Error("The requested page width is invalid.");
    if (editor?.original.scan(request.page))
        self.postMessage({ type:"indexed", generation:currentGeneration, requestId:request.requestId,
            page:request.page, objects:editor.state().objects, sourceNotice:editor.original.notice() });
    const size = pageSizes[request.page];
    const scale = Math.min(request.width / size.width,
                          MAX_DIMENSION / Math.max(size.width, size.height),
                          Math.sqrt(MAX_PIXELS / (size.width * size.height)));
    const width = Math.max(1, Math.floor(size.width * scale));
    const height = Math.max(1, Math.floor(size.height * scale));
    let page = 0;
    let bitmap = 0;
    try {
        page = engine._FPDF_LoadPage(currentDocument, request.page);
        if (!page) throw new Error("This PDF page could not be loaded.");
        bitmap = engine._FPDFBitmap_Create(width, height, 1);
        if (!bitmap) throw new Error("There is not enough browser memory to display this page.");
        if (!engine._FPDFBitmap_FillRect(bitmap, 0, 0, width, height, 0xffffffff))
            throw new Error("This PDF page could not be prepared for display.");
        // FPDF_ANNOT | FPDF_REVERSE_BYTE_ORDER produces RGBA8888, not BGRA.
        // JavaScript actions and interactive form callbacks are never enabled.
        engine._FPDF_RenderPageBitmap(bitmap, page, 0, 0, width, height, 0, 0x01 | 0x10);
        const stride = engine._FPDFBitmap_GetStride(bitmap);
        const pointer = engine._FPDFBitmap_GetBuffer(bitmap);
        if (!pointer || stride < width * 4)
            throw new Error("This PDF page produced an invalid image.");
        const pixels = new Uint8Array(width * height * 4);
        for (let row = 0; row < height; ++row) {
            pixels.set(engine.HEAPU8.subarray(pointer + row * stride,
                                             pointer + row * stride + width * 4), row * width * 4);
        }
        self.postMessage({ type: "rendered", generation: currentGeneration,
                           requestId: request.requestId, page: request.page,
                           width, height, bytes: pixels.buffer }, [pixels.buffer]);
    } finally {
        if (bitmap) engine._FPDFBitmap_Destroy(bitmap);
        if (page) engine._FPDF_ClosePage(page);
    }
}

let queue = Promise.resolve();
self.onmessage = event => {
    const request = event.data;
    if (!request || !Number.isSafeInteger(request.generation) || request.generation < 0) return;
    if (request.type === "open" || request.type === "close")
        newestGeneration = Math.max(newestGeneration, request.generation);
    queue = queue.then(async () => {
        try {
            const engine = await ready;
            // An open/close received while WASM starts invalidates queued older work.
            if (request.generation < newestGeneration) return;
            if (request.type === "open") openDocument(engine, request);
            else if (request.type === "render") renderPage(engine, request);
            else if (request.type === "edit" || request.type === "export") {
                if (!editor || request.generation !== currentGeneration)
                    throw new Error("Open a PDF before editing it.");
                if (!Number.isSafeInteger(request.operationId) || request.operationId < 1)
                    throw new Error("Invalid editing operation.");
                if (request.type === "edit") {
                    const replacement = await editor.edit(request.command);
                    engine._FPDF_CloseDocument(currentDocument);
                    engine._free(currentBytes);
                    currentDocument = replacement.document;
                    currentBytes = replacement.bytes;
                    self.postMessage({ type: "edited", generation: currentGeneration,
                        operationId: request.operationId, ...editor.state() });
                } else {
                    const bytes = editor.export(currentDocument);
                    self.postMessage({ type: "exported", generation: currentGeneration,
                        operationId: request.operationId, ...editor.state(), bytes }, [bytes]);
                }
            }
            else if (request.type === "close") closeDocument(engine);
            else throw new Error("Unsupported PDF operation.");
        } catch (error) {
            self.postMessage({ type: "error", generation: request.generation,
                               requestId: request.requestId,
                               operationId: request.operationId,
                               message: error instanceof Error ? error.message : "The PDF operation failed." });
        }
    });
};
