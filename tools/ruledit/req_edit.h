/***********************************************************************
 Freeciv - Copyright (C) 1996 - A Kjeldberg, L Gregersen, P Unold
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
***********************************************************************/

#ifndef FC__REQ_EDIT_H
#define FC__REQ_EDIT_H

#ifdef HAVE_CONFIG_H
#include <fc_config.h>
#endif

// Qt
#include <QDialog>

// common
#include "requirements.h"

class QListWidget;
class QListWidgetItem;
class QMenu;
class QSpinBox;
class QToolButton;

class ruledit_gui;

class req_edit : public QDialog
{
  Q_OBJECT

  public:
    explicit req_edit(ruledit_gui *ui_in, QString target,
                      struct requirement_vector *preqs);
    void refresh();
    void add(const char *msg);

    struct requirement_vector *req_vector;

signals:
  /********************************************************************//**
    A requirement vector may have been changed.
    @param vec the requirement vector that was changed.
  ************************************************************************/
  void rec_vec_may_have_changed(const requirement_vector *vec);

  private:
    ruledit_gui *ui;

    QListWidget *req_list;

    struct requirement *selected;
    struct requirement selected_values;
    void clear_selected();
    void update_selected();
    void refresh_item(QListWidgetItem *item, struct requirement *preq);
    void refresh_selected();

    QToolButton *edit_type_button;
    QToolButton *edit_value_enum_button;
    QMenu *edit_value_enum_menu;
    QSpinBox *edit_value_nbr_field;
    QToolButton *edit_range_button;
    QToolButton *edit_present_button;

  private slots:
    void select_req();
    void fill_active();
    void add_now();
    void delete_now();
    void close_now();

    void req_type_menu(QAction *action);
    void req_range_menu(QAction *action);
    void req_present_menu(QAction *action);
    void univ_value_enum_menu(QAction *action);
    void univ_value_edit(int value);

    void incoming_rec_vec_change(const requirement_vector *vec);

  protected:
    void closeEvent(QCloseEvent *event);
};

#endif // FC__REQ_EDIT_H
