import '../foliate-js/view.js'

let backend;
window.onload = () => {
    new QWebChannel(qt.webChannelTransport, (channel) => {
        backend = channel.objects.backend;
        dispatch({ type: 'ready' })
        backend.progressChanged.connect(() => {
            if (!rendition.location || rendition.location.start.percentage !== backend.progress) {
                rendition.display(book.locations.cfiFromPercentage(backend.progress))
            }
        })
    })
}

const dispatch = action => {
    if (backend) {
        backend.dispatch(action)
    } else {
        console.error('Dispatch called before backend initialization', new Error().stack)
    }
}

const isZip = async file => {
    const arr = new Uint8Array(await file.slice(0, 4).arrayBuffer())
    return arr[0] === 0x50 && arr[1] === 0x4b && arr[2] === 0x03 && arr[3] === 0x04
}

const isPDF = async file => {
    const arr = new Uint8Array(await file.slice(0, 5).arrayBuffer())
    return arr[0] === 0x25
        && arr[1] === 0x50 && arr[2] === 0x44 && arr[3] === 0x46
        && arr[4] === 0x2d
}

const makeZipLoader = async file => {
    const { configure, ZipReader, BlobReader, TextWriter, BlobWriter } =
        await import('../foliate-js/vendor/zip.js')
    configure({ useWebWorkers: false })
    const reader = new ZipReader(new BlobReader(file))
    const entries = await reader.getEntries()
    const map = new Map(entries.map(entry => [entry.filename, entry]))
    const load = f => (name, ...args) =>
        map.has(name) ? f(map.get(name), ...args) : null
    const loadText = load(entry => entry.getData(new TextWriter()))
    const loadBlob = load((entry, type) => entry.getData(new BlobWriter(type)))
    const getSize = name => map.get(name)?.uncompressedSize ?? 0
    return { entries, loadText, loadBlob, getSize }
}

const isCBZ = ({ name, type }) =>
    type === 'application/vnd.comicbook+zip' || name.endsWith('.cbz')

const isFB2 = ({ name, type }) =>
    type === 'application/x-fictionbook+xml' || name.endsWith('.fb2')

const isFBZ = ({ name, type }) =>
    type === 'application/x-zip-compressed-fb2'
    || name.endsWith('.fb2.zip') || name.endsWith('.fbz')

const open = async file => {
    if (!file.size) {
        emit({ type: 'book-error', payload: 'not-found' })
        return
    }

    let book
    if (await isZip(file)) {
        const loader = await makeZipLoader(file)
        const { entries } = loader
        if (isCBZ(file)) {
            const { makeComicBook } = await import('../foliate-js/comic-book.js')
            book = makeComicBook(loader, file)
        } else if (isFBZ(file)) {
            const { makeFB2 } = await import('../foliate-js/fb2.js')
            const entry = entries.find(entry => entry.filename.endsWith('.fb2'))
            const blob = await loader.loadBlob((entry ?? entries[0]).filename)
            book = await makeFB2(blob)
        } else {
            const { EPUB } = await import('../foliate-js/epub.js')
            book = await new EPUB(loader).init()
        }
    }
    else if (await isPDF(file)) {
        const { makePDF } = await import('../foliate-js/pdf.js')
        book = await makePDF(file)
    }
    else {
        const { isMOBI, MOBI } = await import('../foliate-js/mobi.js')
        if (await isMOBI(file)) {
            const fflate = await import('../foliate-js/vendor/fflate.js')
            book = await new MOBI({ unzlib: fflate.unzlibSync }).open(file)
        } else if (isFB2(file)) {
            const { makeFB2 } = await import('../foliate-js/fb2.js')
            book = await makeFB2(file)
        }
    }

    if (!book) {
        dispatch({ type: 'book-error', payload: 'unsupported-type' })
        return
    }

    const reader = new Reader(book)
    globalThis.reader = reader
    await reader.init()
    dispatch({ type: 'book-ready', book, reader })
}

// Create the Reader class : init->'foliate-view', handleEvents()