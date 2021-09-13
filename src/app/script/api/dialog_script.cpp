// LibreSprite
// Copyright (C) 2021  LibreSprite contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License version 2 as
// published by the Free Software Foundation.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "base/string.h"
#include "script/script_object.h"
#include "ui/base.h"
#include "ui/close_event.h"
#include "ui/widget.h"
#include "ui/window.h"
#include <iostream>

#include "base/alive.h"
#include "base/bind.h"
#include "base/memory.h"
#include "ui/ui.h"

#include "app/app.h"
#include "app/context.h"
#include "app/modules/gui.h"
#include "app/script/app_scripting.h"
#include "app/task_manager.h"
#include "app/ui/status_bar.h"

#include "widget_script.h"
#include "script/engine.h"

#include <memory>
#include <list>

namespace ui {
class Dialog;
}
namespace dialog {
  using DialogIndex = std::unordered_map<std::string, ui::Dialog*>;
DialogIndex& getDialogIndex() {
  static DialogIndex dialogs;
  return dialogs;
}
}

namespace ui {
class Dialog : public base::IsAlive, public ui::Window {
public:
  Dialog() : ui::Window(ui::Window::WithTitleBar, "Script") {}

  ~Dialog() {
      unlist();
  }

  void unlist() {
    auto& index = dialog::getDialogIndex();
    auto it = index.find(id());
    if (it != index.end() && it->second == this)
        index.erase(it);
  }

  void add(WidgetScriptObject* child) {
    auto nextIsInline = m_isInline;
    switch (child->getDisplayType()) {
    case WidgetScriptObject::DisplayType::Inherit: break;
    case WidgetScriptObject::DisplayType::Block: nextIsInline = m_isInline = false; break;
    case WidgetScriptObject::DisplayType::Inline: nextIsInline = true; break;
    }

    auto ui = static_cast<ui::Widget*>(child->getWrapped());
    if (!ui)
        return;

    if(m_isInline && !m_children.empty()) m_children.back().push_back(ui);
    else m_children.push_back({ui});

    m_isInline = nextIsInline;
  }

  void addBreak() {
      m_isInline = false;
  }

  void build(){
    if (!isAlive() || m_grid)
      return;

    // LibreSprite has closed the window, remove corresponding ScriptObject (this)
    Close.connect([this](ui::CloseEvent&){closeWindow(true, false);});

    if (!id().empty())
        dialog::getDialogIndex()[id()] = this;

    if (m_grid)
        removeChild(m_grid);

    std::size_t maxColumns = 1;
    for (auto& row : m_children)
      maxColumns = std::max(row.size(), maxColumns);

    m_grid = new ui::Grid(maxColumns, false);
    addChild(m_grid);

    for (auto& row : m_children) {
      auto size = row.size();
      auto span = 1 + (maxColumns - row.size());
      for (size_t i = 0; i < size; ++i) {
        m_grid->addChildInCell(row[i], span, 1, ui::HORIZONTAL | ui::VERTICAL);
        span = 1;
      }
    }

    setVisible(true);
    centerWindow();
    openWindow();
  }

  void closeWindow(bool raiseEvent, bool notifyManager){
    if (raiseEvent)
      app::AppScripting::raiseEvent(m_scriptFileName, id() + "_close");

    if (notifyManager)
        manager()->_closeWindow(this, true);

    unlist();

    app::TaskManager::instance().delayed([this]{
        if (isAlive()) delete this;
    });
  }

private:
  bool m_isInline = false;
  std::list<std::vector<ui::Widget*>> m_children;
  std::string m_scriptFileName = app::AppScripting::getFileName();
  ui::Grid* m_grid = nullptr;
  inject<script::Engine> m_engine;
  std::unordered_map<std::string, ui::Widget*> m_namedWidgets;
};
}

class DialogScriptObject : public WidgetScriptObject {
  std::unordered_map<std::string, inject<script::ScriptObject>> m_widgets;
  ui::Widget* build() {
    auto dialog = new ui::Dialog();
    dialog->onShutdown = [this]{m_widget = nullptr;};

    // Scripting engine has finished working, build and show the Window
    inject<script::Engine>{}->afterEval([this](bool success){
        if (!m_widget)
            return;
        auto dialog = getWrapped<ui::Dialog>();
        if (success)
            dialog->build();
        if (!dialog->isVisible()){
            dialog->closeWindow(false, true);
        }
    });

    return dialog;
  }

public:
  DialogScriptObject() {
    addProperty("title",
                [this]{return getWrapped<ui::Dialog>()->text();},
                [this](const std::string& title){
                  getWrapped<ui::Dialog>()->setText(title);
                  return title;
                })
      .documentation("read+write. Sets the title of the dialog window.");

    addMethod("add", &DialogScriptObject::add);

    addMethod("get", &DialogScriptObject::get);

    addFunction("close", [this]{
        getWrapped<ui::Dialog>()->closeWindow(false, true);
        return true;
    });

    addFunction("addLabel", [this](const std::string& text, const std::string& id) {
        auto label = add("label", id);
        if (label)
            label->set("text", text);
        return label;
    });

    addFunction("addButton", [this](const std::string& text, const std::string& id) {
        auto button = add("button", id);
        if (button)
            button->set("text", text);
        return button;
    });

    addFunction("addPaletteListBox", [this](const std::string& id) {
        return add("palettelistbox", id);
    });

    addFunction("addIntEntry", [this](const std::string& text, const std::string& id, int min, int max) {
        auto label = add("label", id + "-label");
        if (label)
            label->set("text", text);
        auto intentry = add("intentry", id);
        if (intentry) {
            intentry->set("min", min);
            intentry->set("max", max);
        }
        return intentry;
    });

    addFunction("addBreak", [this]{getWrapped<ui::Dialog>()->addBreak(); return true;});
  }

  ~DialogScriptObject() {
    if (!m_widget)
      return;
    auto dialog = getWrapped<ui::Dialog>();
    if (!dialog->isAlive())
        return;
    if (!dialog->isVisible())
        dialog->closeWindow(false, false);
  }

  ScriptObject* get(const std::string& id) {
    auto it = m_widgets.find(id);
    return it != m_widgets.end() ? it->second.get() : nullptr;
  }

  ScriptObject* add(const std::string& type, const std::string& id) {
    if (type.empty() || get(id))
      return nullptr;

    auto cleanType = base::string_to_lower(type); // "lAbEl" -> "label"
    auto unprefixedType = cleanType;
    cleanType[0] = toupper(cleanType[0]);         // "label" -> "Label"
    cleanType += "WidgetScriptObject";            // "Label" -> "LabelWidgetScriptObject"

    inject<script::ScriptObject> widget{cleanType};
    if (!widget)
        return nullptr;

    auto rawPtr = widget.get<WidgetScriptObject>();
    getWrapped<ui::Dialog>()->add(rawPtr);

    auto cleanId = !id.empty() ? id : unprefixedType + std::to_string(m_nextWidgetId++);
    widget->set("id", cleanId);
    m_widgets.emplace(cleanId, std::move(widget));
    return rawPtr;
  }

  uint32_t m_nextWidgetId = 0;
};

static script::ScriptObject::Regular<DialogScriptObject> dialogSO("DialogScriptObject");

namespace dialog {
ui::Widget* getDialogById(const std::string& id) {
    auto& index = getDialogIndex();
    auto it = index.find(id);
    return it == index.end() ? nullptr : it->second;
}
}
