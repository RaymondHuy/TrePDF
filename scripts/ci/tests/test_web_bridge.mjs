// Exercise the browser/Qt boundary with deterministic worker and timer doubles.
// node --test scripts/ci/tests/test_web_bridge.mjs
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';

const source = readFileSync(new URL('../../../src/web/browser-bridge.js', import.meta.url), 'utf8');
const flush = () => new Promise(resolve => setImmediate(resolve));

function harness(overrides = {}) {
    const messages = [];
    const pixels = [];
    const workers = [];
    const elements = [];
    const timers = new Map();
    const urls = [];
    const revoked = [];
    const allocations = new Set();
    const listeners = new Map();
    let address = 8;
    let timerId = 0;
    const runtime = {
        HEAPU8: new Uint8Array(1024 * 1024),
        _malloc(size) {
            const pointer = address;
            address += size + 8;
            allocations.add(pointer);
            return pointer;
        },
        _free(pointer) {
            assert.ok(allocations.delete(pointer), 'each allocation is freed exactly once');
        },
        _trepdfWebMessage(pointer) {
            const end = runtime.HEAPU8.indexOf(0, pointer);
            messages.push(JSON.parse(new TextDecoder().decode(runtime.HEAPU8.slice(pointer, end))));
        },
        _trepdfWebPixels(requestId, generation, width, height, pointer) {
            pixels.push({ requestId, generation, width, height,
                bytes: runtime.HEAPU8.slice(pointer, pointer + width * height * 4) });
        }
    };
    class FakeWorker {
        constructor(url) {
            this.url = String(url);
            this.posts = [];
            this.transfers = [];
            this.terminated = false;
            workers.push(this);
        }
        postMessage(message, transfer = []) {
            this.transfers.push(transfer.length);
            this.posts.push(structuredClone(message, { transfer }));
        }
        terminate() { this.terminated = true; }
        respond(message) { this.onmessage({ data: message }); }
    }
    class BrowserURL extends URL {
        static createObjectURL(blob) {
            urls.push(blob);
            return `blob:test-${urls.length}`;
        }
        static revokeObjectURL(url) { revoked.push(url); }
    }
    const sandbox = {
        Module: runtime, TextEncoder, Uint8Array, ArrayBuffer, Blob,
        URL: BrowserURL, Worker: FakeWorker,
        addEventListener(event, callback) { listeners.set(event, callback); },
        removeEventListener(event, callback) {
            if (listeners.get(event) === callback) listeners.delete(event);
        },
        setTimeout(callback, delay) {
            timers.set(++timerId, { callback, delay });
            return timerId;
        },
        clearTimeout(id) { timers.delete(id); },
        document: {
            baseURI: 'https://example.test/nested/trepdf/index.html',
            body: { appendChild(element) { element.attached = true; } },
            createElement(tag) {
                const element = {
                    tag, style: {}, files: [], listeners: new Map(),
                    setAttribute(key, value) { this[key] = value; },
                    addEventListener(event, callback) { this.listeners.set(event, callback); },
                    click() { this.clicked = true; },
                    remove() { this.removed = true; }
                };
                elements.push(element);
                return element;
            }
        }
    };
    Object.assign(sandbox, overrides);
    vm.runInNewContext(source, sandbox, { filename: 'browser-bridge.js' });
    const bridge = runtime.trepdfWeb;
    return {
        bridge, runtime, messages, pixels, workers, elements, timers, urls, revoked, allocations, listeners,
        async select(file) {
            bridge.chooseFile();
            const input = elements.at(-1);
            assert.equal(input.tag, 'input');
            input.files = [file];
            input.listeners.get('change')();
            await flush();
            return input;
        },
        fire(delay) {
            const entry = [...timers].find(([, timer]) => timer.delay === delay);
            assert.ok(entry, `a ${delay} ms timeout is armed`);
            timers.delete(entry[0]);
            entry[1].callback();
        },
        opened() {
            const worker = workers.at(-1);
            const generation = worker.posts.find(message => message.type === 'open').generation;
            worker.respond({ type: 'opened', generation, canEdit: true, pages: [{ width: 200, height: 100 }] });
            return { worker, generation };
        }
    };
}

function file(bytes = '%PDF-1.7 original bytes', name = 'original.pdf') {
    const blob = new Blob([bytes], { type: 'application/pdf' });
    Object.defineProperty(blob, 'name', { value: name });
    return blob;
}

test('initialize resolves a nested worker URL and clears the idle startup watchdog', () => {
    const h = harness();
    h.bridge.initialize();
    assert.equal(h.workers[0].url, 'https://example.test/nested/trepdf/pdfium/pdfium-worker.js');
    assert.equal(h.timers.size, 1);
    h.workers[0].respond({ type: 'ready' });
    assert.deepEqual(h.messages, [{ type: 'ready', generation: 0 }]);
    assert.equal(h.timers.size, 0);
    assert.equal(h.allocations.size, 0);
});

test('generation -1 engine startup errors are fatal and cancel the startup watchdog', () => {
    const h = harness();
    h.bridge.initialize();
    h.workers[0].respond({ type: 'error', generation: -1, message: 'Missing engine file.' });
    assert.equal(h.messages.at(-1).message, 'Missing engine file.');
    assert.equal(h.messages.at(-1).fatal, true);
    assert.equal(h.workers[0].terminated, true);
    assert.equal(h.timers.size, 0);
});

test('empty and oversized files are rejected before reading their bytes', async () => {
    const h = harness();
    let reads = 0;
    for (const size of [0, 64 * 1024 * 1024 + 1]) {
        await h.select({ name: 'invalid.pdf', size, arrayBuffer() { ++reads; throw new Error(); } });
        assert.match(h.messages.at(-1).message, /64 MiB/);
        assert.equal(h.messages.at(-1).fatal, undefined);
    }
    assert.equal(reads, 0);
    assert.equal(h.workers.length, 0);
});

test('selected bytes transfer once; ready during open retains its watchdog; download uses original Blob', async () => {
    const h = harness();
    const original = file('exact\0binary\xffcontents', 'Vietnamese tên.pdf');
    const input = await h.select(original);
    assert.equal(input.removed, true);
    const worker = h.workers[0];
    assert.equal(worker.transfers[0], 1);
    assert.deepEqual(new Uint8Array(worker.posts[0].bytes), new Uint8Array(await original.arrayBuffer()));
    worker.respond({ type: 'ready' });
    assert.equal(h.timers.size, 1, 'opening remains bounded after engine readiness');
    h.bridge.downloadFile();
    assert.equal(h.urls.length, 0, 'downloads require a successfully opened document');
    h.opened();
    assert.equal(h.timers.size, 0);
    h.bridge.downloadFile();
    assert.equal(h.urls[0], original, 'download reuses the original Blob without reserializing PDF content');
    const link = h.elements.at(-1);
    assert.equal(link.download, original.name);
    assert.equal(link.href, 'blob:test-1');
    assert.ok(link.clicked && link.removed);
    h.fire(1000);
    assert.deepEqual(h.revoked, ['blob:test-1']);
});

test('raster bytes reach Qt before freeing memory, and one render is active at a time', async () => {
    const h = harness();
    await h.select(file());
    const { worker, generation } = h.opened();
    h.bridge.render(1, 0, 200);
    h.bridge.render(2, 1, 200);
    assert.equal(worker.posts.length, 2, 'second render waits in queue');
    const rgba = new Uint8Array([255, 1, 2, 255, 3, 4, 5, 255]);
    worker.respond({ type: 'rendered', generation, requestId: 1, width: 2, height: 1, bytes: rgba.buffer });
    assert.equal(h.pixels.length, 1);
    assert.deepEqual(h.pixels[0].bytes, rgba);
    assert.equal(h.pixels[0].generation, generation);
    assert.equal(worker.posts.at(-1).requestId, 2);
    assert.equal(h.allocations.size, 0);
});

test('cancelling active and queued pages suppresses callbacks and advances the queue', async () => {
    const h = harness();
    await h.select(file());
    const { worker, generation } = h.opened();
    h.bridge.render(10, 0, 200);
    h.bridge.render(11, 1, 200);
    h.bridge.render(12, 2, 200);
    h.bridge.cancel(10);
    h.bridge.cancel(11);
    worker.respond({ type: 'rendered', generation, requestId: 10, width: 1, height: 1, bytes: new ArrayBuffer(4) });
    assert.equal(h.pixels.length, 0);
    assert.equal(worker.posts.at(-1).requestId, 12);
    assert.equal(worker.posts.some(message => message.requestId === 11), false);
});

test('render queue is bounded and reports rejected requests without losing the document', async () => {
    const h = harness();
    await h.select(file());
    h.opened();
    for (let requestId = 1; requestId <= 66; ++requestId) h.bridge.render(requestId, requestId, 200);
    assert.equal(h.messages.at(-1).requestId, 66);
    assert.match(h.messages.at(-1).message, /Too many pages/);
    assert.equal(h.messages.at(-1).fatal, undefined);
    h.bridge.downloadFile();
    assert.equal(h.urls.length, 1);
});

test('replacement and close ignore terminated workers and stale generation responses', async () => {
    const h = harness();
    await h.select(file());
    const first = h.opened();
    h.bridge.render(1, 0, 200);
    await h.select(file('replacement', 'second.pdf'));
    const second = h.opened();
    assert.equal(first.worker.terminated, true);
    const count = h.messages.length;
    first.worker.respond({ type: 'error', generation: first.generation, message: 'stale' });
    first.worker.onerror();
    second.worker.respond({ type: 'error', generation: first.generation, message: 'old generation' });
    assert.equal(h.messages.length, count);
    h.bridge.closeFile();
    assert.equal(second.worker.terminated, true);
    second.worker.respond({ type: 'opened', generation: second.generation, pages: [] });
    assert.equal(h.messages.at(-1).type, 'closed');
    h.bridge.downloadFile();
    assert.equal(h.urls.length, 0);
    assert.equal(h.timers.size, 0);
});

test('closing during an asynchronous file read prevents a late worker from opening', async () => {
    const h = harness();
    let resolve;
    await h.select({ name: 'slow.pdf', size: 3, arrayBuffer: () => new Promise(done => { resolve = done; }) });
    h.bridge.closeFile();
    resolve(new ArrayBuffer(3));
    await flush();
    assert.equal(h.workers.length, 0);
    assert.equal(h.messages.at(-1).type, 'closed');
});

test('a wrong request ID cannot cancel the active render timeout', async () => {
    const h = harness();
    await h.select(file());
    const { worker, generation } = h.opened();
    h.bridge.render(100, 0, 200);
    worker.respond({ type: 'rendered', generation, requestId: 99, width: 1, height: 1, bytes: new ArrayBuffer(4) });
    assert.equal(h.timers.size, 1);
    h.fire(30000);
    assert.equal(worker.terminated, true);
    assert.equal(h.messages.at(-1).fatal, true);
});

test('startup, open and render timeouts terminate the worker and reject downloads', async () => {
    for (const stage of ['startup', 'open', 'render']) {
        const h = harness();
        if (stage === 'startup') h.bridge.initialize();
        else {
            await h.select(file());
            if (stage === 'render') { h.opened(); h.bridge.render(1, 0, 200); }
        }
        h.fire(30000);
        assert.equal(h.workers[0].terminated, true);
        assert.match(h.messages.at(-1).message, /too long/);
        assert.equal(h.messages.at(-1).fatal, true);
        h.bridge.downloadFile();
        assert.equal(h.urls.length, 0);
    }
});

test('malformed render payloads are rejected before copying them to Qt', async () => {
    const h = harness();
    await h.select(file());
    const { worker, generation } = h.opened();
    h.bridge.render(1, 0, 200);
    worker.respond({ type: 'rendered', generation, requestId: 1, width: 4097, height: 1, bytes: new ArrayBuffer(4) });
    assert.equal(h.pixels.length, 0);
    assert.equal(h.messages.at(-1).fatal, true);
    assert.equal(worker.terminated, true);
});

test('picker cancellation and dispose remove DOM nodes and outstanding work', () => {
    const h = harness();
    h.bridge.initialize();
    h.bridge.chooseFile();
    h.bridge.chooseFile();
    assert.equal(h.elements.length, 1, 'only one picker is active');
    const first = h.elements[0];
    first.listeners.get('cancel')();
    assert.equal(first.removed, true);
    h.bridge.chooseFile();
    assert.equal(h.elements.length, 2);
    h.bridge.dispose();
    assert.equal(h.elements[1].removed, true);
    assert.equal(h.workers[0].terminated, true);
    assert.equal(h.timers.size, 0);
});

test('rejected edits preserve active and queued rasters without leaving pages loading', async () => {
    const h = harness();
    await h.select(file());
    const { worker, generation } = h.opened();
    h.bridge.render(1, 0, 100);
    h.bridge.render(2, 0, 120);
    assert.equal(h.bridge.edit(JSON.stringify({action: 'add', object: {type: 'text'}})), true);
    assert.equal(h.bridge.edit(JSON.stringify({action: 'undo'})), false, 'one pending mutation');
    worker.respond({type: 'rendered', generation, requestId: 1, width: 1, height: 1,
        bytes: new Uint8Array([1, 2, 3, 4]).buffer});
    assert.equal(h.pixels.length, 1, 'active raster finishes before the edit starts');
    const edit = worker.posts.at(-1);
    assert.equal(edit.type, 'edit');
    assert.equal(worker.posts.filter(p => p.requestId === 2).length, 0);
    worker.respond({type: 'error', generation, operationId: edit.operationId, message: 'Text is too large.'});
    assert.equal(h.messages.at(-1).fatal, undefined);
    assert.equal(worker.terminated, false, 'a rejected edit keeps the document alive');
    assert.equal(worker.posts.at(-1).requestId, 2, 'queued page resumes after a rejected mutation');
    worker.respond({type: 'rendered', generation, requestId: 2, width: 1, height: 1,
        bytes: new Uint8Array([1, 2, 3, 4]).buffer});
    assert.deepEqual(h.pixels.map(p => p.requestId), [1, 2]);
});

test('successful edits replace queued rasters at the Qt reset boundary', async () => {
    const h = harness();
    await h.select(file());
    const {worker, generation} = h.opened();
    h.bridge.render(1, 0, 100);
    h.bridge.render(2, 0, 120);
    h.bridge.edit(JSON.stringify({action: 'add', object: {type: 'text'}}));
    worker.respond({type: 'rendered', generation, requestId: 1, width: 1, height: 1,
        bytes: new Uint8Array([1, 2, 3, 4]).buffer});
    worker.respond({type: 'edited', generation, operationId: worker.posts.at(-1).operationId,
        objects: [{id: 1}], dirty: true, canUndo: true});
    assert.equal(worker.posts.filter(p => p.requestId === 2).length, 0,
        'queued rasters invalidated by the successful edit are not dispatched');
    h.bridge.render(3, 0, 100);
    assert.equal(worker.posts.at(-1).requestId, 3, 'new Qt raster request uses the edited document');
});

test('successful and failed exports both resume queued page renders', async () => {
    for (const success of [true, false]) {
        const h = harness();
        await h.select(file());
        const {worker, generation} = h.opened();
        h.bridge.edit(JSON.stringify({action: 'add', object: {type: 'text'}}));
        worker.respond({type: 'edited', generation, operationId: worker.posts.at(-1).operationId,
            objects: [{id: 1}], dirty: true, canUndo: true});
        h.bridge.render(1, 0, 100);
        h.bridge.render(2, 0, 120);
        h.bridge.downloadFile();
        worker.respond({type: 'rendered', generation, requestId: 1, width: 1, height: 1,
            bytes: new Uint8Array([1, 2, 3, 4]).buffer});
        const operationId = worker.posts.at(-1).operationId;
        assert.equal(worker.posts.at(-1).type, 'export');
        worker.respond(success
            ? {type: 'exported', generation, operationId, dirty: false,
                bytes: new TextEncoder().encode('%PDF-1.7 saved').buffer}
            : {type: 'error', generation, operationId, message: 'Export failed.'});
        assert.equal(worker.posts.at(-1).requestId, 2, 'queued render resumes after export');
        worker.respond({type: 'rendered', generation, requestId: 2, width: 1, height: 1,
            bytes: new Uint8Array([1, 2, 3, 4]).buffer});
        assert.deepEqual(h.pixels.map(p => p.requestId), [1, 2]);
    }
});

test('modified download uses verified export bytes and later clean downloads keep the saved PDF', async () => {
    const h = harness();
    await h.select(file());
    const { worker, generation } = h.opened();
    h.bridge.edit(JSON.stringify({action: 'add', object: {type: 'text'}}));
    worker.respond({type: 'edited', generation, operationId: worker.posts.at(-1).operationId,
        objects: [{id: 1}], dirty: true, canUndo: true, canRedo: false, selectedId: 1});
    h.bridge.downloadFile();
    const exportRequest = worker.posts.at(-1);
    assert.equal(exportRequest.type, 'export');
    assert.equal(h.urls.length, 0, 'no original-file download while edits are pending');
    const bytes = new TextEncoder().encode('%PDF-1.7 edited').buffer;
    worker.respond({type: 'exported', generation, operationId: exportRequest.operationId,
        objects: [{id: 1}], dirty: false, canUndo: true, canRedo: false, bytes});
    assert.equal(await h.urls.at(-1).text(), '%PDF-1.7 edited');
    assert.equal(h.elements.at(-1).download, 'original-edited.pdf');
    assert.equal('bytes' in h.messages.at(-1), false, 'PDF bytes never enter Qt JSON state');
    h.bridge.downloadFile();
    assert.equal(h.urls[0], h.urls[1]);
    assert.equal(h.allocations.size, 0);
});

test('failed export retains drafts for retry, read-only documents cannot submit edits', async () => {
    const h = harness();
    await h.select(file());
    const { worker, generation } = h.opened();
    h.bridge.edit(JSON.stringify({action: 'add', object: {type: 'text'}}));
    worker.respond({type: 'edited', generation, operationId: worker.posts.at(-1).operationId,
        objects: [{id: 1}], dirty: true, canUndo: true});
    h.bridge.downloadFile();
    worker.respond({type: 'error', generation, operationId: worker.posts.at(-1).operationId,
        message: 'The PDF could not be saved.'});
    assert.equal(h.urls.length, 0);
    h.bridge.downloadFile();
    assert.equal(worker.posts.at(-1).type, 'export', 'retry never falls back to original bytes');
    h.bridge.closeFile();
    await h.select(file());
    const readonlyWorker = h.workers.at(-1);
    readonlyWorker.respond({type: 'opened', generation: 3, pages: [{width: 200, height: 100}], canEdit: false});
    assert.equal(h.bridge.edit(JSON.stringify({action: 'add'})), false);
});

test('leaving the browser warns for pending input and unsaved edits, and clears after save or close', async () => {
    const h = harness();
    h.bridge.initialize();
    const warns = () => {
        let prevented = false;
        const event = {preventDefault() { prevented = true; }};
        h.listeners.get('beforeunload')(event);
        assert.equal(event.returnValue === '', prevented);
        return prevented;
    };
    assert.equal(warns(), false);
    await h.select(file());
    const {worker, generation} = h.opened();
    h.bridge.setPendingInput(true);
    assert.equal(warns(), true, 'unapplied text is protected');
    h.bridge.setPendingInput(false);
    assert.equal(warns(), false);
    h.bridge.render(1, 0, 100);
    h.bridge.edit(JSON.stringify({action: 'add', object: {type: 'text'}}));
    assert.equal(warns(), true, 'a first edit waiting for a render is protected');
    worker.respond({type: 'rendered', generation, requestId: 1, width: 1, height: 1,
        bytes: new Uint8Array([1, 2, 3, 4]).buffer});
    assert.equal(warns(), true, 'an in-flight first edit is protected');
    worker.respond({type: 'edited', generation, operationId: worker.posts.at(-1).operationId,
        objects: [{id: 1}], dirty: true, canUndo: true});
    assert.equal(warns(), true, 'applied unsaved objects are protected');
    h.bridge.downloadFile();
    worker.respond({type: 'error', generation, operationId: worker.posts.at(-1).operationId,
        message: 'Export failed.'});
    assert.equal(warns(), true, 'failed export retains the guard');
    h.bridge.downloadFile();
    worker.respond({type: 'exported', generation, operationId: worker.posts.at(-1).operationId,
        objects: [{id: 1}], dirty: false, bytes: new TextEncoder().encode('%PDF-1.7 saved').buffer});
    assert.equal(warns(), false, 'successful download clears the saved draft guard');
    h.bridge.setPendingInput(true);
    h.bridge.closeFile();
    assert.equal(warns(), false);
    h.bridge.dispose();
    assert.equal(h.listeners.has('beforeunload'), false);
});


test('source metadata keeps the active raster watchdog and accepts a canceled-page catalog', async () => {
    const h = harness();
    await h.select(file());
    const {worker, generation} = h.opened();
    h.bridge.render(40, 0, 100);
    h.bridge.render(41, 1, 100);
    h.bridge.cancel(40);
    worker.respond({type: 'indexed', generation: generation - 1, requestId: 40, objects: []});
    worker.respond({type: 'indexed', generation, requestId: 99, objects: []});
    assert.equal(h.messages.filter(m => m.type === 'indexed').length, 0);
    worker.respond({type: 'indexed', generation, requestId: 40,
        objects: [{id: 1000000, source: true, type: 'image', page: 0}]});
    assert.equal(h.messages.at(-1).objects[0].id, 1000000);
    assert.equal(worker.posts.at(-1).requestId, 40, 'metadata does not start another render');
    assert.ok([...h.timers.values()].some(timer => timer.delay === 30000));
    worker.respond({type: 'rendered', generation, requestId: 40, width: 1, height: 1,
        pixels: new ArrayBuffer(4)});
    assert.equal(worker.posts.at(-1).requestId, 41);
    assert.equal(h.pixels.length, 0, 'canceled page pixels remain suppressed');
});

function imageHarness(bitmapPromise) {
    const bitmap = {width: 2, height: 1, closed: false, close() { this.closed = true; }};
    const rgba = new Uint8ClampedArray([220, 0, 0, 255, 0, 0, 220, 255]);
    const h = harness({
        createImageBitmap: async () => bitmapPromise ? await bitmapPromise : bitmap,
        OffscreenCanvas: class {
            getContext() { return {drawImage() {}, getImageData() { return {data: rgba}; }}; }
        }
    });
    return {...h, bitmap, rgba};
}
async function selectReplacement(h, image = new Blob(['image'], {type: 'image/png'})) {
    assert.equal(h.bridge.replaceImage(1000000), true);
    const input = h.elements.at(-1);
    assert.equal(input.accept, 'image/png,image/jpeg,image/webp');
    input.files = [image];
    input.listeners.get('change')();
    await flush();
}

test('replacement transfers bounded RGBA pixels and preserves normal operation history', async () => {
    const h = imageHarness();
    await h.select(file());
    const {worker, generation} = h.opened();
    await selectReplacement(h);
    const op = worker.posts.at(-1);
    assert.equal(op.type, 'edit');
    assert.equal(op.command.action, 'replaceImage');
    assert.equal(op.command.object.id, 1000000);
    assert.deepEqual([...new Uint8Array(op.command.image.bytes)], [220, 0, 0, 255, 0, 0, 220, 255]);
    assert.equal(worker.transfers.at(-1), 1);
    assert.equal(h.rgba.byteLength, 0, 'RGBA ownership transferred to worker');
    assert.equal(h.bitmap.closed, true);
    assert.ok(h.messages.some(m => m.type === 'editing'));
    assert.equal(h.bridge.replaceImage(1000000), false, 'second picker blocked during edit');
    worker.respond({type: 'edited', generation, operationId: op.operationId,
        dirty: true, canUndo: true, objects: [{id: 1000000, type: 'image', hasReplacement: true}]});
    assert.equal(h.messages.at(-1).objects[0].hasReplacement, true);
    h.bridge.downloadFile();
    assert.equal(worker.posts.at(-1).type, 'export');
});

test('canceling replacement creates no draft; invalid image leaves the PDF usable', async () => {
    const h = imageHarness();
    await h.select(file());
    const {worker} = h.opened();
    const before = worker.posts.length;
    assert.equal(h.bridge.replaceImage(1000000), true);
    h.elements.at(-1).listeners.get('cancel')();
    assert.equal(worker.posts.length, before);
    assert.equal(h.messages.some(m => m.type === 'editing'), false);
    await selectReplacement(h, new Blob(['svg'], {type: 'image/svg+xml'}));
    assert.match(h.messages.at(-1).message, /PNG, JPEG or WebP/);
    assert.equal(worker.terminated, false);
    h.bridge.downloadFile();
    assert.equal(h.urls.length, 1);
    assert.equal(await h.urls[0].text(), '%PDF-1.7 original bytes');
});

test('image decode is guarded on unload and cannot change a closed or replaced PDF', async () => {
    let resolveBitmap;
    const promise = new Promise(resolve => { resolveBitmap = resolve; });
    const h = imageHarness(promise);
    h.bridge.initialize();
    await h.select(file());
    const {worker} = h.opened();
    await selectReplacement(h);
    let prevented = false;
    h.listeners.get('beforeunload')({preventDefault() { prevented = true; }});
    assert.equal(prevented, true);
    assert.equal(h.bridge.edit('{"action":"undo"}'), false);
    h.bridge.closeFile();
    const count = h.messages.length;
    resolveBitmap(h.bitmap);
    await flush();
    assert.equal(h.messages.length, count);
    assert.equal(h.bitmap.closed, true);
    assert.equal(worker.posts.filter(p => p.type === 'edit').length, 0);
});

test('oversized decoded images release the bitmap and allow retry', async () => {
    const h = imageHarness();
    h.bitmap.width = 4097;
    await h.select(file());
    const {worker} = h.opened();
    await selectReplacement(h);
    assert.match(h.messages.at(-1).message, /4 megapixels/);
    assert.equal(h.bitmap.closed, true);
    assert.equal(worker.posts.some(p => p.type === 'edit'), false);
    assert.equal(h.bridge.replaceImage(1000000), true);
});
