// Exercises real PDFium WASM editing, transactional failures and saved PDFs.
// node scripts/ci/tests/test_web_edit_pdfium.mjs /path/to/pdfium/lib
import assert from "node:assert/strict";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Worker } from "node:worker_threads";

const root = fileURLToPath(new URL("../../../", import.meta.url));
const engineDirectory = path.resolve(process.argv[2] || "_deps/web/pdfium/lib");
const wrapper = `
const { parentPort, workerData } = require('node:worker_threads');
const testFs = require('node:fs'), vm = require('node:vm'), path = require('node:path');
const { pathToFileURL } = require('node:url');
global.self = global; global.require = require; global.__dirname = workerData.engineDirectory;
global.location = { href: pathToFileURL(path.join(workerData.engineDirectory, 'pdfium-worker.js')).href };
global.postMessage = (value, transfers) => parentPort.postMessage(value, transfers);
global.fetch = async url => {
    if (!String(url).endsWith('/fonts/LiberationSans-Regular.ttf')) throw new Error('Unexpected network request');
    const bytes = testFs.readFileSync(path.join(workerData.root, 'tests/fixtures/fonts/liberation/LiberationSans-Regular.ttf'));
    return { ok: true, arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) };
};
global.importScripts = name => {
    if (!['./pdfium.js', './pdfium-editor.js', './pdfium-source.js'].includes(name)) throw new Error('Unexpected worker import');
    const base = name === './pdfium.js' ? workerData.engineDirectory : path.join(workerData.root, 'src/web');
    vm.runInThisContext(testFs.readFileSync(path.join(base, name), 'utf8'));
};
parentPort.on('message', request => {
    if (request.type === 'failNextExport') {
        const original = self.Module._FPDF_SaveAsCopy;
        self.Module._FPDF_SaveAsCopy = (...args) => { self.Module._FPDF_SaveAsCopy = original; return 0; };
        self.postMessage({ type: 'failureInjected', operationId: request.operationId });
        return;
    }
    if (request.type !== 'inspect') { self.onmessage({ data: request }); return; }
    const m = self.Module, bytes = new Uint8Array(request.bytes), pointer = m._malloc(bytes.length);
    m.HEAPU8.set(bytes, pointer);
    const document = m._FPDF_LoadMemDocument(pointer, bytes.length, 0);
    const page = m._FPDF_LoadPage(document, 0), textPage = m._FPDFText_LoadPage(page);
    const count = m._FPDFText_CountChars(textPage), textPointer = m._malloc((count + 1) * 2);
    const actual = m._FPDFText_GetText(textPage, 0, count, textPointer);
    const text = String.fromCharCode(...m.HEAPU16.slice(textPointer / 2, textPointer / 2 + Math.max(0, actual - 1)));
    const objects = [];
    for (let i = 0; i < m._FPDFPage_CountObjects(page); ++i) {
        const object = m._FPDFPage_GetObject(page, i);
        objects.push({ type: m._FPDFPageObj_GetType(object),
            segments: m._FPDFPageObj_GetType(object) === 2 ? m._FPDFPath_CountSegments(object) : 0 });
    }
    m._free(textPointer); m._FPDFText_ClosePage(textPage); m._FPDF_ClosePage(page);
    m._FPDF_CloseDocument(document); m._free(pointer);
    self.postMessage({ type: 'inspected', operationId: request.operationId, text, objects });
});
vm.runInThisContext(testFs.readFileSync(path.join(workerData.root, 'src/web/pdfium-worker.js'), 'utf8'));
`;

function fixture({ rotate = 0, signature = false, crop = false } = {}) {
    const content = "BT /F1 18 Tf 30 350 Td (Original source text) Tj ET\n";
    const objects = [
        `<< /Type /Catalog /Pages 2 0 R ${signature ? "/AcroForm << /Fields [6 0 R] /SigFlags 3 >>" : ""} >>`,
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        `<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 400] /Rotate ${rotate}
            ${crop ? "/CropBox [30 20 570 380]" : ""}
            /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>`,
        `<< /Length ${content.length} >>\nstream\n${content}endstream`,
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"
    ];
    if (signature) objects.push("<< /FT /Sig /T (Signature) /V 7 0 R >>",
        "<< /Type /Sig /Filter /Adobe.PPKLite /SubFilter /adbe.pkcs7.detached /ByteRange [0 1 2 3] /Contents <0000> >>");
    let pdf = "%PDF-1.7\n";
    const offsets = [0];
    objects.forEach((object, index) => { offsets.push(pdf.length); pdf += `${index + 1} 0 obj\n${object}\nendobj\n`; });
    const start = pdf.length;
    pdf += `xref\n0 ${objects.length + 1}\n0000000000 65535 f \n`;
    for (const offset of offsets.slice(1)) pdf += `${String(offset).padStart(10, "0")} 00000 n \n`;
    pdf += `trailer\n<< /Size ${objects.length + 1} /Root 1 0 R >>\nstartxref\n${start}\n%%EOF\n`;
    return new TextEncoder().encode(pdf).buffer;
}

const worker = new Worker(wrapper, { eval: true, workerData: { engineDirectory, root } });
const messages = [];
let notify, workerError;
worker.on("message", message => { messages.push(message); notify?.(); });
worker.on("error", error => { workerError = error; notify?.(); });
async function receive(predicate) {
    const deadline = Date.now() + 15000;
    for (;;) {
        if (workerError) throw workerError;
        const index = messages.findIndex(predicate);
        if (index >= 0) return messages.splice(index, 1)[0];
        if (Date.now() > deadline) throw new Error("PDF worker timeout: " + JSON.stringify(messages));
        await new Promise(resolve => {
            const timer = setTimeout(resolve, 100);
            notify = () => { clearTimeout(timer); resolve(); };
        });
    }
}
let operationId = 0, generation = 1;
async function send(request) {
    request = { generation, operationId: ++operationId, ...request };
    worker.postMessage(request, request.bytes ? [request.bytes] : []);
    return receive(message => request.type === "open" ? message.generation === request.generation
        : request.type === "render" ? message.type !== "indexed" && message.requestId === request.requestId
            : message.operationId === request.operationId);
}
const edit = command => send({ type: "edit", command });
const text = { type: "text", page: 0, x: .1, y: .2, width: .7, height: .18,
    text: "Xin chào Việt Nam", fontSize: 24, color: "#204060" };
const shape = { type: "rectangle", page: 0, x: .2, y: .55, width: .3, height: .25,
    strokeWidth: 3, color: "#ff0000" };

try {
    await receive(message => message.type === "ready");
    let state = await send({ type: "open", bytes: fixture() });
    assert.equal(state.type, "opened"); assert.equal(state.canEdit, true); assert.equal(state.dirty, false);
    state = await edit({ action: "add", object: text });
    assert.equal(state.type, "edited", state.message); assert.equal(state.objects.length, 1);
    assert.equal(state.objects[0].text, text.text); assert.equal(state.dirty, true);
    const textId = state.selectedId;
    state = await edit({ action: "add", object: shape });
    assert.equal(state.type, "edited", state.message); const shapeId = state.selectedId;
    const exported = await send({ type: "export" });
    assert.equal(exported.type, "exported", exported.message); assert.equal(exported.dirty, false);
    const inspected = await send({ type: "inspect", bytes: exported.bytes });
    assert.match(inspected.text, /Original source text/); assert.match(inspected.text, /Xin chào Việt Nam/);
    assert.deepEqual(inspected.objects.map(object => object.type), [1, 1, 2]);
    const source = await send({ type: "inspect", bytes: fixture() });
    assert.equal(source.objects.length, 1, "original PDF remains unchanged");

    state = await edit({ action: "update", object: { ...text, id: textId, text: "Nội dung mới", x: .2 } });
    assert.equal(state.type, "edited", state.message); assert.equal(state.dirty, true);
    await send({ type: "failNextExport" });
    state = await send({ type: "export" }); assert.equal(state.type, "error", "save failure preserves drafts");
    state = await edit({ action: "undo" }); assert.equal(state.dirty, false); assert.equal(state.canRedo, true);
    state = await edit({ action: "redo" }); assert.equal(state.objects[0].text, "Nội dung mới");
    state = await edit({ action: "remove", object: { id: shapeId } }); assert.equal(state.objects.length, 1);
    state = await edit({ action: "discard" });
    assert.equal(state.objects.length, 2); assert.equal(state.objects[0].text, text.text);
    assert.equal(state.dirty, false); assert.equal(state.canUndo, false);

    for (const object of [ { ...text, text: "😀" }, { ...text, text: "中文" },
        { ...text, text: "a".repeat(1025) }, { ...text, width: .001 }, { ...text, height: .001 },
        { ...text, x: .95 }, { ...text, page: 1 }, { ...shape, strokeWidth: 100 }, { ...shape, color: "red" } ]) {
        const failure = await edit({ action: "add", object });
        assert.equal(failure.type, "error", "invalid input rejected transactionally");
    }
    state = await send({ type: "export" }); assert.equal(state.objects.length, 2);
    assert.equal(state.dirty, false, "failed edits keep saved state");
    const afterFailures = await send({ type: "inspect", bytes: state.bytes });
    assert.match(afterFailures.text, /Xin chào Việt Nam/); assert.equal(afterFailures.objects.length, 3);

    for (const type of ["ellipse", "line", "arrow", "triangle", "diamond"]) {
        state = await edit({ action: "add", object: { ...shape, type } });
        assert.equal(state.type, "edited", state.message);
    }
    state = await send({ type: "export" });
    const allShapes = await send({ type: "inspect", bytes: state.bytes });
    assert.equal(allShapes.objects.filter(object => object.type === 2).length, 6);
    assert.ok(allShapes.objects.find(object => object.segments >= 13), "ellipse contains cubic path segments");

    for (const rotate of [0, 90, 180, 270]) {
        ++generation;
        state = await send({ type: "open", bytes: fixture({ rotate, crop: true }) });
        assert.equal(state.type, "opened");
        state = await edit({ action: "add", object: { ...shape, x: .25, y: .25, width: .5, height: .5 } });
        assert.equal(state.type, "edited", state.message);
        const raster = await send({ type: "render", requestId: operationId + 1, page: 0, width: 600 });
        const pixels = new Uint8Array(raster.bytes);
        const x = Math.round(raster.width * .25), y = Math.round(raster.height * .5);
        const pixel = (y * raster.width + x) * 4;
        assert.ok(pixels[pixel] > 200 && pixels[pixel + 1] < 50,
            `normalized placement follows crop/rotation ${rotate}`);
    }
    ++generation;
    state = await send({ type: "open", bytes: fixture({ signature: true }) });
    assert.equal(state.canEdit, false); assert.match(state.editReason, /Signed/);
    state = await edit({ action: "add", object: shape }); assert.equal(state.type, "error");
    state = await send({ type: "render", requestId: operationId + 1, page: 0, width: 200 });
    assert.equal(state.type, "rendered", "signed PDFs remain viewable");

    ++generation;
    await send({ type: "open", bytes: fixture() });
    for (let i = 0; i < 128; ++i) {
        state = await edit({ action: "add", object: shape });
        assert.equal(state.type, "edited", state.message);
    }
    state = await edit({ action: "add", object: shape }); assert.equal(state.type, "error");
    assert.match(state.message, /128/);
    let undos = 0;
    do { state = await edit({ action: "undo" }); ++undos; } while (state.canUndo);
    assert.equal(undos, 31, "history retains at most 32 draft states");
    assert.equal(state.objects.length, 97);
    state = await edit({ action: "discard" }); assert.equal(state.objects.length, 0);
    assert.equal(state.dirty, false, "discard returns to original even after history eviction");
    console.log("PDFium web edit: Unicode text, all shapes, saved PDF reopen, source preservation, undo/redo/discard, validation and rotated/cropped placement passed.");
} finally { await worker.terminate(); }
