#pragma once

namespace OS::EditorUI {
    void OnOpen();   // begin staging from the active outfit
    void OnClose();  // discard staging if the user never pressed Apply
    void Draw();     // called inside the ImGui frame (Present-hook thread)
}
