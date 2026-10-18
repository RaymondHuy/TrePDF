// Run against the packaged site, with Playwright available through NODE_PATH.
// NODE_PATH=/path/to/node_modules node scripts/ci/tests/test_web_browser.cjs build/web-site
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const http = require('node:http');
const path = require('node:path');
const { chromium } = require('playwright');
const { PNG } = require('pngjs');

const site = path.resolve(process.argv[2] || 'build/web-site');
const screenshots = process.env.TREPDF_WEB_SCREENSHOTS;

function fixture({ sourceObjects = false } = {}) {
    const objects = [
        '<< /Type /Catalog /Pages 2 0 R >>',
        '<< /Type /Pages /Kids [4 0 R 6 0 R 8 0 R] /Count 3 >>',
        '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>'
    ];
    ['0.85 0.25 0.2', '0.2 0.6 0.35', '0.2 0.4 0.85'].forEach((color, index) => {
        const originalContent = sourceObjects && index === 0
            ? '0 0 0 rg BT /F1 20 Tf 120 500 Td (Original editable text) Tj ET\nq 120 0 0 90 360 420 cm /Im1 Do Q\n' : '';
        const content = `${color} rg 0 0 600 800 re f\n1 1 1 rg BT /F1 32 Tf 60 700 Td (TrePDF browser test - Page ${index + 1}) Tj ET\n1 0.8 0 rg 60 600 60 100 re f\n${originalContent}`;
        objects.push(`<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] /Resources << /Font << /F1 3 0 R >> ${sourceObjects ? '/XObject << /Im1 10 0 R >>' : ''} >> /Contents ${5 + index * 2} 0 R >>`,
            `<< /Length ${content.length} >>\nstream\n${content}endstream`);
    });
    if (sourceObjects) {
        const pixels = '00C8C800C8C8C800C8C800C8>';
        objects.push(`<< /Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /ASCIIHexDecode /Length ${pixels.length} >>\nstream\n${pixels}\nendstream`);
    }
    let pdf = '%PDF-1.7\n';
    const offsets = [];
    objects.forEach((object, index) => {
        offsets.push(pdf.length);
        pdf += `${index + 1} 0 obj\n${object}\nendobj\n`;
    });
    const start = pdf.length;
    pdf += `xref\n0 ${objects.length + 1}\n0000000000 65535 f \n`;
    for (const offset of offsets) pdf += `${String(offset).padStart(10, '0')} 00000 n \n`;
    pdf += `trailer\n<< /Size ${objects.length + 1} /Root 1 0 R >>\nstartxref\n${start}\n%%EOF\n`;
    return Buffer.from(pdf);
}

// Serve only the staged website, never the checkout or SDK.
const server = http.createServer(async (request, response) => {
    try {
        const pathname = decodeURIComponent(new URL(request.url, 'http://localhost').pathname);
        const filename = path.resolve(site, '.' + (pathname === '/' ? '/index.html' : pathname));
        if (!filename.startsWith(site + path.sep)) throw new Error('Outside site');
        const content = await fs.readFile(filename);
        const types = { '.wasm': 'application/wasm', '.js': 'text/javascript', '.html': 'text/html' };
        response.setHeader('Content-Type', types[path.extname(filename)] || 'application/octet-stream');
        response.end(content);
    } catch (_) { response.writeHead(404); response.end(); }
});

const additions = state => state.objects.filter(object => !object.source);

function replacementPng() {
    const image = new PNG({ width: 16, height: 12 });
    for (let y = 0; y < image.height; y++) {
        for (let x = 0; x < image.width; x++) {
            const rgba = y < image.height / 2 ? [248, 180, 0, 255] : [112, 40, 210, 255];
            image.data.set(rgba, (y * image.width + x) * 4);
        }
    }
    return PNG.sync.write(image);
}

(async () => {
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    let browser, page;
    try {
        browser = await chromium.launch({ headless: true,
            ...(process.env.TREPDF_TEST_CHROME_CHANNEL ? { channel: process.env.TREPDF_TEST_CHROME_CHANNEL } : {}) });
        page = await browser.newPage({ viewport: { width: 1100, height: 800 }, acceptDownloads: true });
        const errors = [];
        page.on('pageerror', error => { errors.push(error.message); console.error(error.stack); });
        page.on('console', message => {
            const text = message.text();
            if (/qrc:.*\.qml|QQml|QML .*:|Binding loop|Cannot assign|ReferenceError|TypeError/.test(text)) {
                errors.push(text);
                console.error(text);
            }
        });
        // Observe real Worker traffic without changing the application or render replies.
        await page.addInitScript(() => {
            window.pdfMessages = [];
            const OriginalWorker = window.Worker;
            window.Worker = class extends OriginalWorker {
                constructor(...args) {
                    super(...args);
                    this.addEventListener('message', event => {
                        const { bytes, ...message } = event.data;
                        window.pdfMessages.push(message);
                    });
                }
            };
        });
        await page.goto(`http://127.0.0.1:${server.address().port}/`);
        const waitFor = async (type, generation, targetPage) => page.waitForFunction(
            ({ type, generation, targetPage }) => window.pdfMessages.some(message =>
                message.type === type && (generation === undefined || message.generation === generation)
                && (targetPage === undefined || message.page === targetPage)),
            { type, generation, targetPage }, { timeout: 60000 });
        await waitFor('ready');
        const pdf = fixture();
        async function choose(name, buffer) {
            await page.waitForTimeout(150); // Let the preceding Qt close/open state reach the canvas.
            const chooser = page.waitForEvent('filechooser');
            // Qt Quick renders controls in a canvas; use the stable desktop toolbar layout.
            await page.mouse.click(54, 62);
            await (await chooser).setFiles({ name, mimeType: 'application/pdf', buffer });
        }
        async function screenshot(name) {
            if (!screenshots) return;
            await fs.mkdir(screenshots, { recursive: true });
            await page.mouse.move(1080, 300);
            await page.screenshot({ path: path.join(screenshots, name + '.png') });
        }
        async function matchesPageColor(expected) {
            // Check the actual Qt canvas, not just a successful worker message.
            const image = PNG.sync.read(await page.screenshot());
            const offset = (400 * image.width + 500) * 4;
            const actual = [...image.data.subarray(offset, offset + 3)];
            return actual.every((value, index) => Math.abs(value - expected[index]) < 8);
        }
        async function assertPageColor(expected) {
            for (let retry = 0; retry < 30; retry++) {
                if (await matchesPageColor(expected)) return;
                await page.waitForTimeout(100);
            }
            await screenshot('unexpected-color');
            assert.fail(`visible PDF color should be ${expected}`);
        }
        async function markerBounds() {
            const image = PNG.sync.read(await page.screenshot());
            let left = image.width, top = image.height, right = -1, bottom = -1;
            for (let y = 160; y < image.height - 50; y++) {
                for (let x = 100; x < image.width - 100; x++) {
                    const offset = (y * image.width + x) * 4;
                    if (Math.abs(image.data[offset] - 255) > 12
                        || Math.abs(image.data[offset + 1] - 204) > 12
                        || image.data[offset + 2] > 12) continue;
                    left = Math.min(left, x); right = Math.max(right, x);
                    top = Math.min(top, y); bottom = Math.max(bottom, y);
                }
            }
            return right < left ? null : { left, top, width: right - left + 1, height: bottom - top + 1 };
        }
        async function waitForMarker(landscape) {
            for (let retry = 0; retry < 30; retry++) {
                const bounds = await markerBounds();
                if (bounds && Math.max(bounds.width, bounds.height) <= 65
                    && Math.max(bounds.width, bounds.height) >= 40
                    && (landscape ? bounds.width > bounds.height * 1.4
                    : bounds.height > bounds.width * 1.4)) return bounds;
                await page.waitForTimeout(100);
            }
            await screenshot('unexpected-rotation');
            assert.fail('PDF marker must rotate with the rendered page');
        }
        async function pageBounds() {
            // Locate the real red PDF page after the property panel resizes the
            // viewport. The fixture's color does not occur in the app chrome.
            const image = PNG.sync.read(await page.screenshot());
            let left = image.width, top = image.height, right = -1, bottom = -1;
            for (let y = 155; y < image.height - 50; y++) {
                for (let x = 0; x < image.width; x++) {
                    const offset = (y * image.width + x) * 4;
                    if (![217, 64, 51].every((value, index) =>
                        Math.abs(image.data[offset + index] - value) < 8)) continue;
                    left = Math.min(left, x); right = Math.max(right, x);
                    top = Math.min(top, y); bottom = Math.max(bottom, y);
                }
            }
            assert.ok(right > left && bottom > top, 'the first PDF page is visible');
            return { left, top, width: right - left + 1, height: bottom - top + 1 };
        }
        async function editStateAfter(action) {
            const start = await page.evaluate(() => window.pdfMessages.length);
            await action();
            await page.waitForFunction(start => window.pdfMessages.slice(start).some(message =>
                message.type === 'edited' || message.type === 'error'), start, { timeout: 30000 });
            const result = await page.evaluate(start => window.pdfMessages.slice(start).find(message =>
                message.type === 'edited' || message.type === 'error'), start);
            assert.equal(result.type, 'edited', result.message || 'UI edit reaches the PDF worker');
            await page.waitForTimeout(180);
            return result;
        }
        async function typeInlineText(text) {
            // Placement focuses the on-page TextArea immediately. No click or
            // keyboard focus in the properties panel should be necessary.
            await page.waitForTimeout(150);
            // Exercise a real composition/commit cycle. A bare insertText
            // command skips composition events when Qt's hidden DOM input has
            // an existing selection, unlike an OS Vietnamese input method.
            const cdp = await page.context().newCDPSession(page);
            try {
                await cdp.send('Input.imeSetComposition', { text,
                    selectionStart: text.length, selectionEnd: text.length });
                await cdp.send('Input.insertText', { text });
            } finally {
                await cdp.detach();
            }
        }
        async function applyInlineText() {
            await page.keyboard.press('Control+Enter');
        }
        async function objectPoint(object) {
            const bounds = await pageBounds();
            return { x: bounds.left + (object.x + object.width / 2) * bounds.width,
                y: bounds.top + (object.y + object.height / 2) * bounds.height };
        }
        async function originalObjects() {
            const generation = await page.evaluate(() => window.pdfMessages.filter(message =>
                message.type === 'opened').at(-1).generation);
            await waitFor('indexed', generation, 0);
            return page.evaluate(generation => window.pdfMessages.filter(message =>
                message.type === 'indexed' && message.generation === generation && message.page === 0).at(-1).objects
                .filter(object => object.page === 0),
            generation);
        }
        async function openAtEdit(name, bytes) {
            await page.mouse.click(45, 18);
            const start = await page.evaluate(() => window.pdfMessages.length);
            await choose(name, bytes);
            await page.waitForFunction(start => window.pdfMessages.slice(start).some(message =>
                message.type === 'opened'), start, { timeout: 30000 });
            const generation = await page.evaluate(start => window.pdfMessages.slice(start).find(message =>
                message.type === 'opened').generation, start);
            await waitFor('rendered', generation, 0);
            await page.waitForTimeout(250); // Let Qt process opened before sending toolbar clicks.
            await page.mouse.click(715, 781);
            await page.mouse.click(1054, 781);
            await page.keyboard.press('Home');
            await page.keyboard.press('ArrowDown');
            await page.keyboard.press('ArrowDown');
            await page.keyboard.press('Enter');
            await page.mouse.click(135, 18);
            const objects = await originalObjects();
            await page.waitForTimeout(200);
            return objects;
        }
        async function saveEdited() {
            const ready = page.waitForEvent('download');
            await page.mouse.click(1040, 169);
            const result = await ready;
            return fs.readFile(await result.path());
        }
        async function assertReplacementPixels(object) {
            for (let retry = 0; retry < 30; retry++) {
                const bounds = await pageBounds();
                const image = PNG.sync.read(await page.screenshot());
                const colors = [[248, 180, 0], [112, 40, 210]];
                const match = colors.every((color, index) => {
                    const x = Math.round(bounds.left + (object.x + object.width / 2) * bounds.width);
                    const y = Math.round(bounds.top + (object.y + object.height * (index ? 0.75 : 0.25)) * bounds.height);
                    const offset = (y * image.width + x) * 4;
                    // PDFium's image/color conversion can shift RGB channels
                    // slightly; these two colors remain distinct from source.
                    return color.every((value, channel) => Math.abs(image.data[offset + channel] - value) < 20);
                });
                if (match) return;
                await page.waitForTimeout(100);
            }
            assert.fail('the real PDF canvas displays both replacement image colors');
        }
        async function assertPersistedObjectPixels(objects) {
            const bounds = await pageBounds();
            const image = PNG.sync.read(await page.screenshot());
            for (const object of objects) {
                let darkerPixels = 0;
                const left = Math.max(0, Math.floor(bounds.left + object.x * bounds.width) - 2);
                const top = Math.max(0, Math.floor(bounds.top + object.y * bounds.height) - 2);
                const right = Math.min(image.width, Math.ceil(left + object.width * bounds.width) + 4);
                const bottom = Math.min(image.height, Math.ceil(top + object.height * bounds.height) + 4);
                for (let y = top; y < bottom; y++) {
                    for (let x = left; x < right; x++) {
                        const offset = (y * image.width + x) * 4;
                        if (image.data[offset] < 185 && image.data[offset + 1] < 80
                                && image.data[offset + 2] < 80) darkerPixels++;
                    }
                }
                assert.ok(darkerPixels >= 15, `reopened ${object.type} has visible pixels in its saved frame`);
            }
        }
        await page.waitForTimeout(300);
        await screenshot('empty');
        assert.deepEqual(errors, [], 'QML starts without warnings');
        await choose('browser-test.pdf', pdf);
        await waitFor('opened', 1);
        await waitFor('rendered', 1, 0);
        await page.waitForTimeout(300);
        await assertPageColor([217, 64, 51]);
        await screenshot('page-one');

        const downloadReady = page.waitForEvent('download');
        await page.mouse.click(218, 62);
        const download = await downloadReady;
        assert.equal(download.suggestedFilename(), 'browser-test.pdf');
        assert.deepEqual(await fs.readFile(await download.path()), pdf, 'download preserves every original byte');

        // Unsupported tabs must leave the shared View ribbon active.
        for (const x of [220, 300, 385, 470, 555, 640, 725, 810])
            await page.mouse.click(x, 18);
        await page.mouse.click(398, 62);
        await waitFor('rendered', 1, 1);
        await assertPageColor([51, 153, 89]);
        await page.mouse.click(316, 62);
        await assertPageColor([217, 64, 51]);

        await page.mouse.move(700, 500);
        // Native wheel scaling differs between Linux Chromium and macOS Chrome.
        for (let step = 0; step < 8; step++) {
            await page.mouse.wheel(0, 800);
            await page.waitForTimeout(200);
            if (await matchesPageColor([51, 153, 89])) break;
        }
        await waitFor('rendered', 1, 1);
        await page.waitForTimeout(300);
        await assertPageColor([51, 153, 89]);
        await screenshot('page-two');
        // Zoom out generates a new raster size, including for the visible page.
        const beforeZoom = await page.evaluate(() => window.pdfMessages.filter(m => m.type === 'rendered').length);
        await page.mouse.click(496, 62);
        await page.waitForFunction(count => window.pdfMessages.filter(m => m.type === 'rendered').length > count,
            beforeZoom, { timeout: 15000 });
        await page.waitForTimeout(200);
        await assertPageColor([51, 153, 89]);
        await screenshot('zoom');

        await page.mouse.click(398, 62); // Next page
        await waitFor('rendered', 1, 2);
        await page.waitForTimeout(200);
        await assertPageColor([51, 102, 217]);

        // Shared footer: Single Page keeps wheel navigation inside one page.
        await page.mouse.click(715, 781);
        await page.mouse.click(20, 781); // First page
        await assertPageColor([217, 64, 51]);
        await page.mouse.move(700, 500);
        await page.mouse.wheel(0, 5000);
        await page.waitForTimeout(300);
        await assertPageColor([217, 64, 51]);

        // 50% shows the full page; the asymmetric gold marker verifies actual
        // scene-graph rotation, including a turn that reuses the same raster.
        await page.mouse.click(1054, 781);
        await page.keyboard.press('Home');
        await page.keyboard.press('ArrowDown');
        await page.keyboard.press('ArrowDown');
        await page.keyboard.press('Enter');
        const originalMarker = await waitForMarker(false);
        await screenshot('single-page');
        await page.mouse.click(771, 62);
        const rotatedMarker = await waitForMarker(true);
        assert.ok(Math.abs(rotatedMarker.width - originalMarker.height) <= 3);
        assert.ok(Math.abs(rotatedMarker.height - originalMarker.width) <= 3);
        await screenshot('rotated');
        await page.mouse.click(853, 62);
        const restoredMarker = await waitForMarker(false);
        assert.ok(Math.abs(restoredMarker.left - originalMarker.left) <= 3);
        assert.ok(Math.abs(restoredMarker.top - originalMarker.top) <= 3);

        await page.mouse.click(745, 781); // Continuous
        await page.mouse.click(775, 781); // Facing is unavailable in this preview
        await page.mouse.click(805, 781); // Continuous Facing is also unavailable
        await page.mouse.click(161, 781); // Shared footer Next page
        await assertPageColor([51, 153, 89]);
        await screenshot('continuous');

        await page.mouse.click(170, 131); // Close document tab
        await choose('invalid.pdf', Buffer.from('not a PDF'));
        await waitFor('error', 3);
        await choose('browser-test.pdf', pdf);
        await waitFor('rendered', 4, 0);
        await page.waitForTimeout(200);
        await page.mouse.click(715, 781); // Single Page, then 50% for a complete visible page.
        await page.mouse.click(1054, 781);
        await page.keyboard.press('Home');
        await page.keyboard.press('ArrowDown');
        await page.keyboard.press('ArrowDown');
        await page.keyboard.press('Enter');
        await page.mouse.click(135, 18); // Edit is now enabled.
        await page.mouse.click(218, 62); // Add Text, using the shared Edit ribbon.
        await page.waitForTimeout(200);
        let bounds = await pageBounds();
        await page.mouse.click(bounds.left + bounds.width * 0.2, bounds.top + bounds.height * 0.35);
        await typeInlineText('Xin chào Việt Nam');
        const beforeTypingKeys = await page.evaluate(() => window.pdfMessages.length);
        // Backspace and Undo belong to the text editor, not the PDF history.
        await page.keyboard.press('Backspace');
        await page.keyboard.press('ControlOrMeta+z');
        await page.waitForTimeout(150);
        assert.equal(await page.evaluate(start => window.pdfMessages.slice(start).some(message =>
            message.type === 'edited'), beforeTypingKeys), false,
            'typing Backspace/Undo must not remove the object or change PDF history');
        await screenshot('pending-vietnamese-text');
        let state = await editStateAfter(applyInlineText);
        assert.equal(additions(state).length, 1);
        assert.equal(additions(state)[0].text, 'Xin chào Việt Nam');
        assert.equal(state.dirty, true);
        assert.equal(state.canUndo, true);
        await screenshot('edit-vietnamese-text');

        // Existing session text opens the same inline editor on double-click.
        // Escape abandons only that pending change; the committed text remains.
        let textPoint = await objectPoint(additions(state)[0]);
        const beforeCanceledEdit = await page.evaluate(() => window.pdfMessages.length);
        await page.mouse.dblclick(textPoint.x, textPoint.y, { delay: 100 });
        await page.waitForTimeout(150);
        await screenshot('cancel-inline-before-typing');
        await page.keyboard.press('ControlOrMeta+a');
        await page.keyboard.type('Do not save');
        await screenshot('cancel-inline-after-typing');
        await page.keyboard.press('Escape');
        await page.waitForTimeout(150);
        assert.equal(await page.evaluate(start => window.pdfMessages.slice(start).some(message =>
            message.type === 'edited'), beforeCanceledEdit), false,
            'Escape cancels inline text without committing it');
        textPoint = await objectPoint(additions(state)[0]);
        await page.mouse.dblclick(textPoint.x, textPoint.y, { delay: 100 });
        await page.waitForTimeout(150);
        await screenshot('update-inline-before-typing');
        await page.keyboard.press('ControlOrMeta+a');
        await typeInlineText('Chào bạn Việt Nam');
        await screenshot('update-inline-after-typing');
        state = await editStateAfter(applyInlineText);
        assert.equal(additions(state).length, 1, 'inline update keeps the same addition');
        assert.equal(additions(state)[0].text, 'Chào bạn Việt Nam');
        await screenshot('updated-inline-text');

        const beforeMove = additions(state)[0];
        let point = await objectPoint(beforeMove);
        state = await editStateAfter(async () => {
            await page.mouse.move(point.x, point.y);
            await page.mouse.down();
            await page.mouse.move(point.x + 30, point.y + 24, { steps: 8 });
            await page.mouse.up();
        });
        assert.ok(additions(state)[0].x > beforeMove.x + 0.05, 'drag moves the added text');
        point = await objectPoint(additions(state)[0]);
        state = await editStateAfter(async () => {
            await page.mouse.click(point.x, point.y);
            await page.keyboard.press('Backspace');
        });
        assert.equal(additions(state).length, 0, 'Delete removes the selected addition');
        state = await editStateAfter(() => page.mouse.click(884, 62)); // Undo
        assert.equal(additions(state).length, 1, 'Undo restores the addition');
        state = await editStateAfter(() => page.mouse.click(966, 62)); // Redo
        assert.equal(additions(state).length, 0, 'Redo repeats the deletion');
        state = await editStateAfter(() => page.mouse.click(884, 62));
        assert.equal(additions(state).length, 1);

        await page.mouse.click(514, 62); // Shapes dropdown
        await page.waitForTimeout(150);
        await page.mouse.click(328, 133); // Rectangle
        await page.waitForTimeout(150);
        bounds = await pageBounds();
        state = await editStateAfter(async () => {
            await page.mouse.move(bounds.left + bounds.width * 0.55, bounds.top + bounds.height * 0.55);
            await page.mouse.down();
            await page.mouse.move(bounds.left + bounds.width * 0.8, bounds.top + bounds.height * 0.72, { steps: 8 });
            await page.mouse.up();
        });
        assert.equal(additions(state).length, 2);
        assert.equal(additions(state)[1].type, 'rectangle');
        await screenshot('edit-text-and-shape');

        // Dirty close/open are guarded before changing generation or opening a
        // browser picker. Escape exercises the dialog's Cancel path.
        let unexpectedChooser = false;
        const onUnexpectedChooser = () => { unexpectedChooser = true; };
        page.on('filechooser', onUnexpectedChooser);
        const messageCount = await page.evaluate(() => window.pdfMessages.length);
        await page.mouse.click(170, 131);
        await page.waitForTimeout(150);
        await screenshot('dirty-close-guard');
        await page.keyboard.press('Escape');
        await page.mouse.click(1080, 131); // Plus/open while dirty.
        await page.waitForTimeout(150);
        await page.keyboard.press('Escape');
        page.off('filechooser', onUnexpectedChooser);
        assert.equal(unexpectedChooser, false, 'dirty Open waits for a decision before showing the file picker');
        assert.equal(await page.evaluate(start => window.pdfMessages.slice(start).some(message =>
            message.type === 'opened'), messageCount), false, 'Cancel retains the current document');

        // Save submits pending text without a separate Apply click, then exports
        // a real modified PDF instead of the original download.
        await page.mouse.click(218, 62);
        bounds = await pageBounds();
        await page.mouse.click(bounds.left + bounds.width * 0.15, bounds.top + bounds.height * 0.75);
        await typeInlineText('Lưu bản thử');
        // Opening the close guard blurs the inline editor. Cancel must retain
        // its contents, and Save must then apply them without another click.
        const beforePendingClose = await page.evaluate(() => window.pdfMessages.length);
        await page.mouse.click(170, 131);
        await page.waitForTimeout(150);
        await screenshot('pending-inline-close-guard');
        await page.keyboard.press('Escape');
        assert.equal(await page.evaluate(start => window.pdfMessages.slice(start).some(message =>
            message.type === 'edited' || message.type === 'opened'), beforePendingClose), false,
            'Cancel keeps pending inline input uncommitted');
        const editedDownloadReady = page.waitForEvent('download');
        await page.mouse.click(1040, 169);
        const editedDownload = await editedDownloadReady;
        assert.equal(editedDownload.suggestedFilename(), 'browser-test-edited.pdf');
        const editedPdf = await fs.readFile(await editedDownload.path());
        assert.notDeepEqual(editedPdf, pdf, 'Save writes the edited PDF');
        assert.equal(editedPdf.subarray(0, 5).toString(), '%PDF-');
        state = await page.evaluate(() => window.pdfMessages.filter(message => message.type === 'exported').at(-1));
        assert.equal(additions(state).length, 3, 'Save applied the pending text first');
        assert.equal(additions(state)[2].text, 'Lưu bản thử');
        assert.equal(state.dirty, false);
        await screenshot('saved-edit');

        await page.mouse.click(45, 18); // View hides editing overlays.
        const beforeReopen = await page.evaluate(() => window.pdfMessages.length);
        await choose('browser-test-edited.pdf', editedPdf);
        await page.waitForFunction(start => window.pdfMessages.slice(start).some(message =>
            message.type === 'opened'), beforeReopen, { timeout: 30000 });
        await page.waitForFunction(start => window.pdfMessages.slice(start).some(message =>
            message.type === 'rendered' && message.page === 0), beforeReopen, { timeout: 30000 });
        const reopened = await page.evaluate(start => window.pdfMessages.slice(start).find(message =>
            message.type === 'opened'), beforeReopen);
        assert.equal(reopened.pages.length, 3);
        assert.deepEqual(additions(reopened), [], 'reopened additions are persisted source content');
        await page.mouse.click(715, 781);
        await page.mouse.click(1054, 781);
        await page.keyboard.press('Home');
        await page.keyboard.press('ArrowDown');
        await page.keyboard.press('ArrowDown');
        await page.keyboard.press('Enter');
        await page.waitForTimeout(200);
        await assertPersistedObjectPixels(additions(state));
        await screenshot('reopened-edited-pdf');

        // Exercise original PDF objects through the same canvas UI, including
        // actual removal/replacement in exported PDF content. The image is an
        // opaque RGB XObject, not a vector rectangle posing as a picture.
        let originals = await openAtEdit('source-objects.pdf', fixture({ sourceObjects: true }));
        const sourceText = originals.find(object => object.type === 'text' && object.text === 'Original editable text');
        let sourceImage = originals.find(object => object.type === 'image');
        assert.ok(sourceText?.source && sourceText.editable, 'original text is an editable source candidate');
        assert.ok(sourceImage?.source && sourceImage.editable, 'original raster image is an editable source candidate');
        await screenshot('original-text-and-image');
        await page.mouse.click(136, 62); // Edit Text opens original text inline.
        point = await objectPoint(sourceText);
        await page.mouse.click(point.x, point.y);
        await page.waitForTimeout(200);
        await page.keyboard.press('ControlOrMeta+a');
        await typeInlineText('Sửa chữ gốc');
        state = await editStateAfter(applyInlineText);
        const changedText = state.objects.find(object => object.id === sourceText.id);
        assert.equal(changedText.text, 'Sửa chữ gốc');
        assert.equal(changedText.source, true, 'editing replaces the selected source, not an unrelated addition');
        await screenshot('source-text-inline-replaced');

        await page.mouse.click(54, 62); // Select
        point = await objectPoint(sourceImage);
        await page.mouse.click(point.x, point.y);
        await page.waitForTimeout(200); // Opening the panel changes the page's screen bounds.
        point = await objectPoint(sourceImage);
        state = await editStateAfter(async () => {
            await page.mouse.move(point.x, point.y);
            await page.mouse.down();
            await page.mouse.move(point.x - 25, point.y + 30, { steps: 8 });
            await page.mouse.up();
        });
        const movedImage = state.objects.find(object => object.id === sourceImage.id);
        assert.ok(movedImage.x < sourceImage.x - 0.04 && movedImage.y > sourceImage.y + 0.04,
            'drag moves the original image');
        bounds = await pageBounds();
        const handle = { x: bounds.left + (movedImage.x + movedImage.width) * bounds.width,
            y: bounds.top + (movedImage.y + movedImage.height) * bounds.height };
        state = await editStateAfter(async () => {
            await page.mouse.move(handle.x, handle.y);
            await page.mouse.down();
            await page.mouse.move(handle.x + 18, handle.y + 14, { steps: 8 });
            await page.mouse.up();
        });
        const resizedImage = state.objects.find(object => object.id === sourceImage.id);
        assert.ok(resizedImage.width > movedImage.width + 0.025
            && resizedImage.height > movedImage.height + 0.015, 'corner handle resizes the source image');
        assert.ok(Math.abs(resizedImage.width / resizedImage.height
            - movedImage.width / movedImage.height) < 0.01, 'image resize preserves the aspect lock');
        await screenshot('source-image-before-replacement');
        state = await editStateAfter(async () => {
            const chooser = page.waitForEvent('filechooser');
            await page.mouse.click(960, 420); // Image properties: Replace image.
            await (await chooser).setFiles({ name: 'replacement.png', mimeType: 'image/png', buffer: replacementPng() });
        });
        const replacedImage = state.objects.find(object => object.id === sourceImage.id);
        assert.equal(replacedImage.hasReplacement, true, 'replacement uses the selected source image');
        for (const field of ['x', 'y', 'width', 'height'])
            assert.equal(replacedImage[field], resizedImage[field], `image replacement preserves ${field}`);
        await assertReplacementPixels(replacedImage);
        await screenshot('source-image-replaced');
        point = await objectPoint(resizedImage);
        state = await editStateAfter(async () => {
            await page.mouse.click(point.x, point.y);
            await page.keyboard.press('Backspace');
        });
        assert.equal(state.objects.some(object => object.id === sourceImage.id), false,
            'Delete removes the original image from the editable canvas');
        state = await editStateAfter(() => page.mouse.click(884, 62));
        assert.ok(state.objects.some(object => object.id === sourceImage.id), 'Undo restores the original image');
        await screenshot('source-image-moved-resized');
        const sourceEditedPdf = await saveEdited();
        if (screenshots) await fs.writeFile(path.join(screenshots, 'source-edited.pdf'), sourceEditedPdf);
        originals = await openAtEdit('source-edited.pdf', sourceEditedPdf);
        assert.equal(originals.some(object => object.type === 'text' && object.text.includes('Original editable text')), false,
            'saving replaces the original PDF text instead of painting over it: ' + JSON.stringify(originals));
        assert.ok(originals.some(object => object.type === 'text' && object.text === 'Sửa chữ gốc'),
            'Vietnamese source replacement survives Save/reopen');
        sourceImage = originals.find(object => object.type === 'image');
        assert.ok(sourceImage, 'the moved image remains a real PDF image');
        for (const field of ['x', 'y', 'width', 'height'])
            assert.ok(Math.abs(sourceImage[field] - resizedImage[field]) < 0.005,
                `saved original image preserves ${field}`);
        await assertReplacementPixels(sourceImage);
        await screenshot('reopened-source-edits');
        point = await objectPoint(sourceImage);
        state = await editStateAfter(async () => {
            await page.mouse.click(point.x, point.y);
            await page.keyboard.press('Backspace');
        });
        const sourceDeletedPdf = await saveEdited();
        originals = await openAtEdit('source-image-deleted.pdf', sourceDeletedPdf);
        assert.equal(originals.some(object => object.type === 'image'), false,
            'source image deletion persists in PDF content after reopening');
        await screenshot('reopened-source-image-deleted');
        await page.mouse.click(45, 18);
        await page.setViewportSize({ width: 768, height: 1024 });
        await page.waitForTimeout(500);
        await assertPageColor([217, 64, 51]);
        await screenshot('tablet');
        assert.deepEqual(errors, [], 'no JavaScript/WebAssembly errors or QML warnings');
        console.log('Browser: shared ribbon/footer, reader controls, disabled tools, byte-exact original download, inline Vietnamese typing/update, text Backspace/Undo/Escape, move/delete/Undo/Redo, shape drawing, dirty guards, pending-input Save, modified download/reopen, original text replacement, source image move/resize/replace/delete/Undo and persistence, invalid-file recovery and resize passed.');
    } catch (error) {
        if (page && screenshots) {
            await fs.mkdir(screenshots, { recursive: true });
            await page.screenshot({ path: path.join(screenshots, 'failure.png') });
            console.error('Last PDF messages:', await page.evaluate(() => window.pdfMessages?.slice(-4)));
        }
        throw error;
    } finally {
        if (browser) await browser.close();
        await new Promise(resolve => server.close(resolve));
    }
})().catch(error => { console.error(error); process.exitCode = 1; });
