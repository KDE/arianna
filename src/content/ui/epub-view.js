let book = ePub("test.epub");
let rendition = book.renderTo("area", { flow: 'paginated', maxSpreadColumns: 2 });
let displayed = rendition.display();

var backend;

book.ready.then(() => {
    backend.locationsReady = false
    book.locations.generate().then((k) => {
        backend.locationsReady = true
        console.log(k)
    })
    rendition.start()
})

window.onload = () => {
    new QWebChannel(qt.webChannelTransport, (channel) => {
        backend = channel.objects.backend;
        backend.progressChanged.connect(() => {
            if (rendition.location.start.percentage !== backend.progress) {
                rendition.display(book.locations.cfiFromPercentage(backend.progress))
            }
        })
    })
}

rendition.on('relocated', () => {
    backend.progress = rendition.location.start.percentage

})

function test() {
    let sectionMarks = book.spine.items.map(section => book.locations
        .percentageFromCfi('epubcfi(' + section.cfiBase + '!/0)'))
    console.log(sectionMarks)
}
