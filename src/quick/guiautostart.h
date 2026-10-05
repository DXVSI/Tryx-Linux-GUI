#pragma once

#include <QString>

namespace gui_autostart {

struct State {
    bool available = false;
    bool enabled = false;
    QString error;
};

State query();
bool setEnabled(bool enabled, QString *error);

namespace testing {

using BeforeUserEntryOpenHook = void (*)(const QString &path);
enum class LeafNamespaceMutation {
    Create,
    Replace,
    Remove,
};
using BeforeLeafNamespaceMutationHook =
    void (*)(LeafNamespaceMutation operation,
             const QString &entryPath);
using AfterLeafPreconditionCheckHook =
    void (*)(LeafNamespaceMutation operation,
             const QString &entryPath);
using AfterLeafNamespaceMutationHook =
    void (*)(LeafNamespaceMutation operation,
             const QString &entryPath);

void setUserConfigDirectoryOverride(const QString &directory);
void clearUserConfigDirectoryOverride();
void setBeforeUserEntryOpenHook(BeforeUserEntryOpenHook hook);
void clearBeforeUserEntryOpenHook();
void setBeforeLeafNamespaceMutationHook(
    BeforeLeafNamespaceMutationHook hook);
void clearBeforeLeafNamespaceMutationHook();
void setAfterLeafPreconditionCheckHook(
    AfterLeafPreconditionCheckHook hook);
void clearAfterLeafPreconditionCheckHook();
void setAfterLeafNamespaceMutationHook(
    AfterLeafNamespaceMutationHook hook);
void clearAfterLeafNamespaceMutationHook();

}  // namespace testing

}  // namespace gui_autostart
