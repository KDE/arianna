from dataclasses import dataclass

from PySide2.QtWidgets import QApplication
from PySide2.QtQml import QQmlApplicationEngine
from PySide2.QtCore import QObject, Slot, Signal, Property, QJsonValue

@dataclass
class PyObject(QObject):

    def __post_init__(self): super().__init__()

    @Slot('QVariant')
    def saveLocations(self, locations):
        with open('.cache', 'w') as file:
            file.write(repr(dict(locations.toVariant())))

    @Slot(result='QVariant')
    def loadLocations(self):
        try:
            with open('.cache') as file:
                return eval(file.read())
        except (FileNotFoundError, SyntaxError):
            return {}

p = PyObject()

app = QApplication()
qml = QQmlApplicationEngine()
qml.rootContext().setContextProperty('py', p)
qml.load('test.qml')
app.exec_()
