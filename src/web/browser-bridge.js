// Included in the Qt runtime with --pre-js. The worker has a separate Wasm
// memory, keeping the native PDF engine off the browser's UI thread.
function createTrePDFBrowserBridge(runtime, browser) {
    const maximumFileBytes = 64 * 1024 * 1024;
    let worker = null;
    let generation = 0;
    let originalFile = null;
    let candidateFile = null;
    let opened = false;
    let picker = null;
    let active = null;
    let timeout = null;
    const queue = new Map();
    const cancelled = new Set();

    function emit(message) {
        const bytes = new TextEncoder().encode(JSON.stringify(message) + '\0');
        const pointer = runtime._malloc(bytes.length);
        if (!pointer) throw new Error('Browser memory is full.');
        try {
            runtime.HEAPU8.set(bytes, pointer);
            runtime._trepdfWebMessage(pointer);
        } finally { runtime._free(pointer); }
    }

    function stopWorker() {
        if (timeout !== null) browser.clearTimeout(timeout);
        timeout = null;
        if (worker) worker.terminate();
        worker = null;
        active = null;
        queue.clear();
        cancelled.clear();
    }

    function fail(message) {
        stopWorker();
        opened = false;
        originalFile = candidateFile = null;
        emit({type: 'error', generation, message, fatal: true});
    }

    function armTimeout() {
        if (timeout !== null) browser.clearTimeout(timeout);
        timeout = browser.setTimeout(() => fail('The PDF took too long to process. Try a smaller file.'), 30000);
    }

    function schedule() {
        if (!worker || active || !opened || queue.size === 0) return;
        const [requestId, request] = queue.entries().next().value;
        queue.delete(requestId);
        active = request;
        armTimeout();
        worker.postMessage(request);
    }

    function createWorker() {
        const current = new browser.Worker(new URL('pdfium/pdfium-worker.js', browser.document.baseURI));
        worker = current;
        current.onerror = () => {
            if (worker === current) fail('The PDF engine could not run. Reload the page and try again.');
        };
        current.onmessageerror = current.onerror;
        current.onmessage = event => {
            if (worker !== current) return;
            const message = event.data;
            if (message.type === 'ready') {
                if (!candidateFile && !active) {
                    if (timeout !== null) browser.clearTimeout(timeout);
                    timeout = null;
                }
                emit({type: 'ready', generation});
                return;
            }
            if (message.type === 'error' && message.generation === -1) {
                fail(message.message);
                return;
            }
            if (message.generation !== generation) return;
            const isRenderReply = message.type === 'rendered' || (message.type === 'error' && message.requestId);
            if (isRenderReply && (!active || message.requestId !== active.requestId)) return;
            if (message.type !== 'opened' && message.type !== 'error' && !isRenderReply) return;
            if (timeout !== null) browser.clearTimeout(timeout);
            timeout = null;
            if (message.type === 'opened') {
                opened = true;
                originalFile = candidateFile;
                candidateFile = null;
                emit(message);
            } else if (isRenderReply) {
                active = null;
                if (!cancelled.delete(message.requestId)) {
                    if (message.type === 'error') {
                        emit(message);
                    } else {
                        const size = message.width * message.height * 4;
                        if (!Number.isInteger(message.width) || !Number.isInteger(message.height)
                            || message.width < 1 || message.height < 1
                            || message.width > 4096 || message.height > 4096 || size > 16000000
                            || !(message.bytes instanceof ArrayBuffer) || message.bytes.byteLength !== size) {
                            fail('The PDF engine returned an invalid page.');
                            return;
                        }
                        const pointer = runtime._malloc(size);
                        if (!pointer) { fail('Browser memory is full. Try a smaller PDF.'); return; }
                        try {
                            runtime.HEAPU8.set(new Uint8Array(message.bytes), pointer);
                            runtime._trepdfWebPixels(message.requestId, generation, message.width, message.height, pointer);
                        } finally { runtime._free(pointer); }
                    }
                }
            } else if (message.type === 'error') {
                fail(message.message);
                return;
            }
            schedule();
        };
        return current;
    }

    async function openSelectedFile(file) {
        if (!file) return;
        if (file.size === 0 || file.size > maximumFileBytes) {
            emit({type: 'error', generation, message: 'Choose a PDF between 1 byte and 64 MiB.'});
            return;
        }
        generation++;
        stopWorker();
        originalFile = null;
        candidateFile = file;
        opened = false;
        const requestedGeneration = generation;
        emit({type: 'loading', generation, fileName: file.name});
        try {
            const bytes = await file.arrayBuffer();
            if (requestedGeneration !== generation) return;
            armTimeout();
            createWorker().postMessage({type: 'open', generation, bytes}, [bytes]);
        } catch (_) {
            if (requestedGeneration === generation) fail('This PDF could not be opened. Please choose it again.');
        }
    }

    return {
        initialize() {
            try { armTimeout(); createWorker(); }
            catch (_) { fail('This browser cannot start the PDF viewer.'); }
        },
        chooseFile() {
            if (picker) return;
            picker = browser.document.createElement('input');
            picker.type = 'file';
            picker.accept = 'application/pdf,.pdf';
            picker.style.display = 'none';
            picker.setAttribute('aria-label', 'Open PDF');
            const input = picker;
            const remove = () => { input.remove(); if (picker === input) picker = null; };
            input.addEventListener('cancel', remove, {once: true});
            input.addEventListener('change', () => {
                const file = input.files[0];
                remove();
                void openSelectedFile(file);
            }, {once: true});
            browser.document.body.appendChild(input);
            input.click();
        },
        closeFile() {
            generation++;
            stopWorker();
            originalFile = candidateFile = null;
            opened = false;
            emit({type: 'closed', generation});
        },
        downloadFile() {
            if (!opened || !originalFile) return;
            const url = browser.URL.createObjectURL(originalFile);
            const link = browser.document.createElement('a');
            link.href = url;
            link.download = originalFile.name;
            browser.document.body.appendChild(link);
            link.click();
            link.remove();
            browser.setTimeout(() => browser.URL.revokeObjectURL(url), 1000);
        },
        render(requestId, page, width) {
            if (!opened) return;
            if (queue.size >= 64) {
                emit({type: 'error', generation, requestId, message: 'Too many pages are waiting. Scroll back to retry.'});
                return;
            }
            queue.set(requestId, {type: 'render', generation, requestId, page, width});
            schedule();
        },
        cancel(requestId) {
            queue.delete(requestId);
            if (active && active.requestId === requestId) cancelled.add(requestId);
        },
        dispose() {
            generation++;
            stopWorker();
            opened = false;
            originalFile = candidateFile = null;
            if (picker) { picker.remove(); picker = null; }
        }
    };
}

Module.trepdfWeb = createTrePDFBrowserBridge(Module, globalThis);
