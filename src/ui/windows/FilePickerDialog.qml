import Eden.Ui
import QtQuick
import QtQuick.Dialogs

Item {
    id: picker

    required property var controller

    function open() {
        const request = controller.fileDialog;
        if (!request || request.id === undefined)
            return ;

        if (request.mode === "folder") {
            folderDialog.title = request.title && request.title.length > 0 ? request.title : "Choose folder";
            folderDialog.open();
            return ;
        }
        fileDialog.fileMode = request.mode === "save" ? FileDialog.SaveFile : request.mode === "openMultiple" ? FileDialog.OpenFiles : FileDialog.OpenFile;
        fileDialog.nameFilters = request.nameFilters && request.nameFilters.length > 0 ? request.nameFilters : ["All files (*)"];
        fileDialog.title = request.title && request.title.length > 0 ? request.title : request.mode === "save" ? "Save file" : "Open file";
        if (request.defaultPath && request.defaultPath.length > 0)
            fileDialog.selectedFile = "file://" + request.defaultPath;

        fileDialog.open();
    }

    function close() {
        fileDialog.close();
        folderDialog.close();
    }

    FileDialog {
        id: fileDialog

        onAccepted: picker.controller.resolveFileDialog(true, selectedFiles)
        onRejected: picker.controller.resolveFileDialog(false, [])
    }

    FolderDialog {
        id: folderDialog

        onAccepted: picker.controller.resolveFileDialog(true, [selectedFolder])
        onRejected: picker.controller.resolveFileDialog(false, [])
    }

}
