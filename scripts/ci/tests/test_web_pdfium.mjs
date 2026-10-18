// Real PDFium smoke/limits tests, without installing Qt or Emscripten locally.
// node scripts/ci/tests/test_web_pdfium.mjs /path/to/pdfium/lib
import assert from "node:assert/strict";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Worker } from "node:worker_threads";

const engineDirectory = path.resolve(process.argv[2] || "_deps/web/pdfium/lib");
const source = fileURLToPath(new URL("../../../src/web/pdfium-worker.js", import.meta.url));
// Adapt the classic Web Worker APIs only; all PDFium calls execute real WASM.
const wrapper = `
const { parentPort, workerData } = require('node:worker_threads');
const testFs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const { pathToFileURL } = require('node:url');
global.self = global;
global.require = require;
global.__dirname = workerData.engineDirectory;
global.location = { href: pathToFileURL(path.join(workerData.engineDirectory, 'pdfium-worker.js')).href };
global.postMessage = (message, transfers) => parentPort.postMessage(message, transfers);
global.importScripts = name => {
    if (!['./pdfium.js', './pdfium-editor.js', './pdfium-source.js'].includes(name)) throw new Error('Unexpected worker import');
    const base = name === './pdfium.js' ? workerData.engineDirectory : path.dirname(workerData.source);
    vm.runInThisContext(testFs.readFileSync(path.join(base, name), 'utf8'));
};
parentPort.on('message', data => self.onmessage({ data }));
vm.runInThisContext(testFs.readFileSync(workerData.source, 'utf8'));
`;

function fixture(pageCount = 1) {
    const content = "1 0 0 rg 0 0 200 100 re f\n";
    const objects = [
        "<< /Type /Catalog /Pages 2 0 R >>",
        `<< /Type /Pages /Kids [${"3 0 R ".repeat(pageCount)}] /Count ${pageCount} >>`,
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Resources << >> /Contents 4 0 R >>",
        `<< /Length ${content.length} >>\nstream\n${content}endstream`
    ];
    let pdf = "%PDF-1.7\n";
    const offsets = [0];
    objects.forEach((object, index) => {
        offsets.push(pdf.length);
        pdf += `${index + 1} 0 obj\n${object}\nendobj\n`;
    });
    const start = pdf.length;
    pdf += `xref\n0 ${objects.length + 1}\n0000000000 65535 f \n`;
    for (const offset of offsets.slice(1)) pdf += `${String(offset).padStart(10, "0")} 00000 n \n`;
    pdf += `trailer\n<< /Size ${objects.length + 1} /Root 1 0 R >>\nstartxref\n${start}\n%%EOF\n`;
    return new TextEncoder().encode(pdf).buffer;
}

const worker = new Worker(wrapper, { eval: true, workerData: { engineDirectory, source } });
const messages = [];
let notify;
worker.on("message", message => { messages.push(message); notify?.(); });
worker.on("error", error => { console.error(error); process.exitCode = 1; notify?.(); });
async function receive(predicate) {
    const deadline = Date.now() + 15000;
    for (;;) {
        const index = messages.findIndex(predicate);
        if (index >= 0) return messages.splice(index, 1)[0];
        if (Date.now() >= deadline) throw new Error("Timed out waiting for PDF worker: " + JSON.stringify(messages));
        await new Promise(resolve => {
            const timer = setTimeout(resolve, 200);
            notify = () => { clearTimeout(timer); resolve(); };
        });
    }
}
async function send(request) {
    worker.postMessage(request, request.bytes ? [request.bytes] : []);
    return receive(message => message.generation === request.generation
        && (request.requestId === undefined || message.type !== "indexed" && message.requestId === request.requestId));
}

try {
    await receive(message => message.type === "ready");
    let result = await send({ type: "open", generation: 1, bytes: fixture() });
    assert.equal(result.type, "opened");
    assert.deepEqual(result.pages, [{ width: 200, height: 100 }]);
    result = await send({ type: "render", generation: 1, requestId: 1, page: 0, width: 200 });
    assert.equal(result.type, "rendered");
    assert.equal(result.width, 200);
    assert.equal(result.height, 100);
    assert.equal(result.bytes.byteLength, 200 * 100 * 4);
    assert.deepEqual([...new Uint8Array(result.bytes).slice(0, 4)], [255, 0, 0, 255], "RGBA byte order");

    result = await send({ type: "render", generation: 1, requestId: 2, page: 0, width: 1e9 });
    assert.equal(result.type, "rendered");
    assert.ok(result.width <= 4096 && result.height <= 4096);
    assert.ok(result.width * result.height <= 4_000_000, "raster allocation cap");
    for (const [requestId, page, width] of [[3, -1, 200], [4, 1, 200], [5, 0, NaN], [6, 0, 0]]) {
        result = await send({ type: "render", generation: 1, requestId, page, width });
        assert.equal(result.type, "error");
    }
    result = await send({ type: "open", generation: 2, bytes: new TextEncoder().encode("not a PDF").buffer });
    assert.equal(result.type, "error");
    assert.match(result.message, /could not be opened/);
    result = await send({ type: "render", generation: 2, requestId: 7, page: 0, width: 100 });
    assert.equal(result.type, "error", "a failed replacement releases the previous document");

    result = await send({ type: "open", generation: 3, bytes: new ArrayBuffer(64 * 1024 * 1024 + 1) });
    assert.equal(result.type, "error");
    assert.match(result.message, /64 MiB/);
    result = await send({ type: "open", generation: 4, bytes: fixture(2001) });
    assert.equal(result.type, "error");
    assert.match(result.message, /2,000 pages/);
    result = await send({ type: "open", generation: 5, bytes: fixture() });
    assert.equal(result.type, "opened", "valid documents recover after invalid input");
    worker.postMessage({ type: "close", generation: 6 });
    result = await send({ type: "render", generation: 6, requestId: 8, page: 0, width: 100 });
    assert.equal(result.type, "error", "close releases the document");
    console.log("PDFium web worker: real WASM render, RGBA, raster/file/page limits, invalid input, recovery and close passed.");
} finally {
    await worker.terminate();
}
