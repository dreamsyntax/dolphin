// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Debugger/CodeDiffDialog.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QVBoxLayout>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpacerItem>
#include <chrono>
#include <regex>
#include <vector>
#include "Common/StringUtil.h"
#include "Core/Core.h"
#include "Core/Debugger/PPCDebugInterface.h"
#include "Core/HW/CPU.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/Profiler.h"
#include "DolphinQt/Debugger/CodeWidget.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/Settings.h"

CodeDiffDialog::CodeDiffDialog(CodeWidget* parent) : QDialog(parent), m_code_widget(parent)
{
  setWindowTitle(tr("Code Diff Tool"));
  CreateWidgets();
  ConnectWidgets();
}

void CodeDiffDialog::reject()
{
  ClearData();
  auto& settings = Settings::GetQSettings();
  settings.setValue(QStringLiteral("diffdialog/geometry"), saveGeometry());
  QDialog::reject();
}

void CodeDiffDialog::CreateWidgets()
{
  auto& settings = Settings::GetQSettings();
  restoreGeometry(settings.value(QStringLiteral("diffdialog/geometry")).toByteArray());
  auto* btns_layout = new QGridLayout;
  m_exclude_btn = new QPushButton(tr("Code did not get executed"));
  m_include_btn = new QPushButton(tr("Code has been executed"));
  m_record_btn = new QPushButton(tr("Start Recording"));
  m_record_btn->setCheckable(true);
  m_record_btn->setStyleSheet(
      QStringLiteral("QPushButton:checked { background-color: rgb(150, 0, 0); border-style: solid; "
                     "border-width: 3px; border-color: rgb(150,0,0); color: rgb(255, 255, 255);}"));

  m_exclude_btn->setEnabled(false);
  m_include_btn->setEnabled(false);

  btns_layout->addWidget(m_exclude_btn, 0, 0);
  btns_layout->addWidget(m_include_btn, 0, 1);
  btns_layout->addWidget(m_record_btn, 0, 2);

  auto* labels_layout = new QHBoxLayout;
  m_exclude_size_label = new QLabel(tr("Excluded: 0"));
  m_include_size_label = new QLabel(tr("Included: 0"));

  btns_layout->addWidget(m_exclude_size_label, 1, 0);
  btns_layout->addWidget(m_include_size_label, 1, 1);

  m_matching_results_list = new QListWidget();
  m_matching_results_list->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  m_matching_results_list->setContextMenuPolicy(Qt::CustomContextMenu);
  m_reset_btn = new QPushButton(tr("Reset All"));
  m_reset_btn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  m_help_btn = new QPushButton(tr("Help"));
  m_help_btn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  auto* help_reset_layout = new QHBoxLayout;
  help_reset_layout->addWidget(m_reset_btn, 0, Qt::AlignLeft);
  help_reset_layout->addWidget(m_help_btn, 0, Qt::AlignRight);

  auto* layout = new QVBoxLayout();
  layout->addLayout(btns_layout);
  layout->addLayout(labels_layout);
  layout->addWidget(m_matching_results_list);
  layout->addLayout(help_reset_layout);

  setLayout(layout);
}

void CodeDiffDialog::ConnectWidgets()
{
  connect(m_record_btn, &QPushButton::toggled, this, &CodeDiffDialog::OnRecord);
  connect(m_include_btn, &QPushButton::pressed, [this]() { Update(true); });
  connect(m_exclude_btn, &QPushButton::pressed, [this]() { Update(false); });
  connect(m_matching_results_list, &QListWidget::itemClicked, [this]() { OnClickItem(); });
  connect(m_reset_btn, &QPushButton::pressed, this, &CodeDiffDialog::ClearData);
  connect(m_help_btn, &QPushButton::pressed, this, &CodeDiffDialog::InfoDisp);
  connect(m_matching_results_list, &CodeDiffDialog::customContextMenuRequested, this,
          &CodeDiffDialog::OnContextMenu);
}

void CodeDiffDialog::OnClickItem()
{
  UpdateItem();
  auto address = m_matching_results_list->currentItem()->data(Qt::UserRole).toUInt();
  m_code_widget->SetAddress(address, CodeViewWidget::SetAddressUpdate::WithDetailedUpdate);
}

void CodeDiffDialog::ClearData()
{
  if (m_record_btn->isChecked())
    m_record_btn->toggle();
  ClearBlockCache();
  m_matching_results_list->clear();
  m_exclude_size_label->setText(QStringLiteral("Excluded: 0"));
  m_include_size_label->setText(QStringLiteral("Included: 0"));
  m_exclude_btn->setEnabled(false);
  m_include_btn->setEnabled(false);
  // Swap is used instead of clear for efficiency in the case of huge m_include/m_exclude
  std::vector<Diff>().swap(m_include);
  std::vector<Diff>().swap(m_exclude);
  JitInterface::SetProfilingState(JitInterface::ProfilingState::Disabled);
}

void CodeDiffDialog::ClearBlockCache()
{
  Core::State old_state = Core::GetState();

  if (old_state == Core::State::Running)
    Core::SetState(Core::State::Paused);

  JitInterface::ClearCache();

  if (old_state == Core::State::Running)
    Core::SetState(Core::State::Running);
}

void CodeDiffDialog::OnRecord(bool enabled)
{
  if (m_failed_requirements)
  {
    m_failed_requirements = false;
    return;
  }

  if (Core::GetState() == Core::State::Uninitialized)
  {
    ModalMessageBox::information(this, tr("Code Diff Tool"),
                                 QStringLiteral("Emulation must be started to record."));
    m_failed_requirements = true;
    m_record_btn->setChecked(false);
    return;
  }

  if (g_symbolDB.IsEmpty())
  {
    ModalMessageBox::warning(
        this, tr("Code Diff Tool"),
        QStringLiteral("Symbol map not found.\n\nIf one does not exist, you can generate one from "
                       "the Menu bar:\nSymbols -> Generate Symbols From ->\n\tAddress | Signature "
                       "Database | RSO Modules"));
    m_failed_requirements = true;
    m_record_btn->setChecked(false);
    return;
  }

  JitInterface::ProfilingState state;

  if (enabled)
  {
    ClearBlockCache();
    m_record_btn->setText(tr("Stop Recording"));
    state = JitInterface::ProfilingState::Enabled;
    m_exclude_btn->setEnabled(true);
    m_include_btn->setEnabled(true);
  }
  else
  {
    ClearBlockCache();
    m_record_btn->setText(tr("Start Recording"));
    state = JitInterface::ProfilingState::Disabled;
    m_exclude_btn->setEnabled(false);
    m_include_btn->setEnabled(false);
  }

  m_record_btn->update();
  JitInterface::SetProfilingState(state);
}

void CodeDiffDialog::OnInclude()
{
  const auto recorded_symbols = CalculateSymbolsFromProfile();
  if (m_include.empty() && m_exclude.empty())
  {
    m_include = recorded_symbols;
    return;
  }

  std::vector<Diff> current_diff;
  if (m_exclude.empty())
  {
    current_diff = recorded_symbols;
  }
  else
  {
    for (auto& iter : recorded_symbols)
    {
      auto pos = std::lower_bound(m_exclude.begin(), m_exclude.end(), iter.symbol);

      if (pos == m_exclude.end() || pos->symbol != iter.symbol)
      {
        current_diff.push_back(iter);
      }
    }
  }

  if (!m_include.empty())
  {
    RemoveMissingSymbolsFromIncludes(current_diff);
  }
  else
  {
    m_include = recorded_symbols;
    RemoveMatchingSymbolsFromIncludes(m_exclude);
  }
}

void CodeDiffDialog::OnExclude()
{
  const auto recorded_symbols = CalculateSymbolsFromProfile();
  if (m_include.empty() && m_exclude.empty())
  {
    m_exclude = recorded_symbols;
    return;
  }

  std::vector<Diff> current_diff;
  if (m_exclude.empty())
  {
    m_exclude = recorded_symbols;
  }
  else
  {
    for (auto& iter : recorded_symbols)
    {
      auto pos = std::lower_bound(m_exclude.begin(), m_exclude.end(), iter.symbol);

      if (pos == m_exclude.end() || pos->symbol != iter.symbol)
      {
        current_diff.push_back(iter);
        m_exclude.insert(pos, iter);
      }
    }
    // If there is no include list, we're done.
    if (m_include.empty())
      return;
  }

  if (!m_exclude.empty())
  {
    RemoveMatchingSymbolsFromIncludes(current_diff);
  }
}

std::vector<Diff> CodeDiffDialog::CalculateSymbolsFromProfile()
{
  Profiler::ProfileStats prof_stats;
  auto& blockstats = prof_stats.block_stats;
  JitInterface::GetProfileResults(&prof_stats);
  std::vector<Diff> current;
  current.reserve(20000);

  // Convert blockstats to smaller struct Diff. Exclude repeat functions via symbols.
  for (auto& iter : blockstats)
  {
    Diff tmp_diff;
    std::string symbol = g_symbolDB.GetDescription(iter.addr);
    if (!std::any_of(current.begin(), current.end(),
                     [&symbol](Diff& v) { return v.symbol == symbol; }))
    {
      tmp_diff.symbol = symbol;
      tmp_diff.addr = iter.addr;
      tmp_diff.hits = iter.run_count;
      current.push_back(tmp_diff);
    }
  }

  sort(current.begin(), current.end(),
       [](const Diff& v1, const Diff& v2) { return (v1.symbol < v2.symbol); });

  return current;
}

void CodeDiffDialog::RemoveMissingSymbolsFromIncludes(const std::vector<Diff>& symbol_diff)
{
  m_include.erase(std::remove_if(m_include.begin(), m_include.end(),
                                 [&](const Diff& v) {
                                   return std::none_of(
                                       symbol_diff.begin(), symbol_diff.end(), [&](const Diff& p) {
                                         return p.symbol == v.symbol || p.addr == v.addr;
                                       });
                                 }),
                  m_include.end());
}

void CodeDiffDialog::RemoveMatchingSymbolsFromIncludes(const std::vector<Diff>& symbol_list)
{
  m_include.erase(std::remove_if(m_include.begin(), m_include.end(),
                                 [&](const Diff& i) {
                                   return std::any_of(
                                       symbol_list.begin(), symbol_list.end(), [&](const Diff& s) {
                                         return i.symbol == s.symbol || i.addr == s.addr;
                                       });
                                 }),
                  m_include.end());
}

void CodeDiffDialog::Update(bool include)
{
  // Wrap everything in a pause
  Core::State old_state = Core::GetState();
  if (old_state == Core::State::Running)
    Core::SetState(Core::State::Paused);

  // Main process
  if (include)
  {
    OnInclude();
  }
  else
  {
    OnExclude();
  }

  m_matching_results_list->clear();

  new QListWidgetItem(tr("Address\tHits\tSymbol"), m_matching_results_list);

  for (auto& iter : m_include)
  {
    QString fix_sym = QString::fromStdString(iter.symbol);
    fix_sym.replace(QStringLiteral("\t"), QStringLiteral("  "));

    QString tmp_out =
        QStringLiteral("%1\t%2\t%3").arg(iter.addr, 1, 16).arg(iter.hits).arg(fix_sym);

    auto* item = new QListWidgetItem(tmp_out, m_matching_results_list);
    item->setData(Qt::UserRole, iter.addr);

    m_matching_results_list->addItem(item);
  }

  m_exclude_size_label->setText(QStringLiteral("Excluded: %1").arg(m_exclude.size()));
  m_include_size_label->setText(QStringLiteral("Included: %1").arg(m_include.size()));

  JitInterface::ClearCache();
  if (old_state == Core::State::Running)
    Core::SetState(Core::State::Running);
}

void CodeDiffDialog::InfoDisp()
{
  ModalMessageBox::information(
      this, tr("Code Diff Tool Help"),
      QStringLiteral(
          "Used to find functions based on when they should be running.\nSimilar to Cheat Engine "
          "Ultimap.\n"
          "A symbol map must be loaded prior to use.\n\n'Start Recording': will "
          "keep track of what functions run. Clicking 'Stop Recording' again will erase current "
          "recording without any change to the lists.\n'Code did not get executed': click while "
          "recording, will add recorded functions to an exclude "
          "list, then reset the recording list.\n'Code has been executed': click while recording, "
          "will add "
          "recorded function to an include list, then reset the recording list.\n\nAfter you use "
          "both "
          "exclude and include once, the exclude list will be subtracted from the include list "
          "and "
          "any includes left over will be displayed.\nYou can continue to use "
          "'Code did not get executed'/'Code has been executed' to narrow down the "
          "results.\n\nExample: "
          "You want to find a function that runs when HP is modified.\n1. Start recording and "
          "play the game without letting HP be modified, then press 'Code did not get "
          "executed'.\n2. "
          "Immediately gain/lose HP and press 'Code has been executed'.\n3. Repeat 1 or 2 to "
          "narrow down the "
          "results.\nIncludes should "
          "have short recordings focusing on what you want.\n\nPressing 'Code has been "
          "executed' twice will only "
          "keep functions that ran for both recordings.\n\nRight click -> 'Set blr' will place a "
          "blr at the top of the symbol.\n"
          "Recording lists will persist on ending emulation / restarting emulation. Recordings "
          "will not persist on Dolphin close."));
}

void CodeDiffDialog::OnContextMenu()
{
  UpdateItem();
  QMenu* menu = new QMenu(this);
  menu->addAction(tr("&Go to start of function"), this, &CodeDiffDialog::OnGoTop);
  menu->addAction(tr("Set &blr"), this, &CodeDiffDialog::OnSetBLR);
  menu->addAction(tr("&Delete"), this, &CodeDiffDialog::OnDelete);
  menu->exec(QCursor::pos());
}

void CodeDiffDialog::OnGoTop()
{
  auto item = m_matching_results_list->currentItem();
  if (!item)
    return;
  Common::Symbol* symbol = g_symbolDB.GetSymbolFromAddr(item->data(Qt::UserRole).toUInt());
  if (!symbol)
    return;
  m_code_widget->SetAddress(symbol->address, CodeViewWidget::SetAddressUpdate::WithDetailedUpdate);
}

void CodeDiffDialog::OnDelete()
{
  // Delete from include and listwidget.
  int remove_item = m_matching_results_list->row(m_matching_results_list->currentItem());
  if (!remove_item || remove_item == -1)
    return;
  m_include.erase(m_include.begin() + remove_item - 1);
  m_matching_results_list->takeItem(remove_item);
}

void CodeDiffDialog::OnSetBLR()
{
  auto item = m_matching_results_list->currentItem();
  if (!item)
    return;
  Common::Symbol* symbol = g_symbolDB.GetSymbolFromAddr(item->data(Qt::UserRole).toUInt());
  if (!symbol)
    return;
  PowerPC::debug_interface.SetPatch(symbol->address, 0x4E800020);
  item->setForeground(QBrush(Qt::red));
  m_code_widget->Update();
}

void CodeDiffDialog::UpdateItem()
{
  auto item = m_matching_results_list->currentItem();
  auto address = item->data(Qt::UserRole).toUInt();
  auto fullstring = item->text().toStdString();
  auto symbolName = g_symbolDB.GetDescription(address);
  int row = m_matching_results_list->row(item);
  if (!row || row == -1)
    return;

  size_t pos = 0;
  std::vector<std::string> v;
  while ((pos = fullstring.find("\t")) != std::string::npos)
  {
    std::string token = fullstring.substr(0, pos);
    v.push_back(token);
    fullstring.erase(0, pos + 1);
  }

  QString fix_sym =
      QString::fromStdString(symbolName).replace(QStringLiteral("\t"), QStringLiteral("  "));
  QString updatedItem = QStringLiteral("%1\t%2\t%3")
                            .arg(address, 1, 16)
                            .arg(QString::fromStdString(v[1]))
                            .arg(fix_sym);

  m_matching_results_list->currentItem()->setText(updatedItem);
}
