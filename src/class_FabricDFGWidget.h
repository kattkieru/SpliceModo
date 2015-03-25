
#ifndef _FABRICDFGWIDGET_H_
#define _FABRICDFGWIDGET_H_

#include <QtGui/QWidget>
#include <QtGui/QSplitter>

#include <DFGWrapper/DFGWrapper.h>
#include <ASTWrapper/KLASTManager.h>
#include <Commands/CommandStack.h>
#include <map>

#include <FabricUI/DFG/DFGUI.h>
#include <FabricUI/DFG/DFGCombinedWidget.h>
#include <FabricUI/DFG/DFGValueEditor.h>

#include <FabricSplice.h>

#include "class_BaseInterface.h"

using namespace FabricServices;
using namespace FabricUI;

class FabricDFGWidget : public DFG::DFGCombinedWidget {

  Q_OBJECT
  
public:
  FabricDFGWidget(QWidget *parent, BaseInterface *baseInterface);
  ~FabricDFGWidget();

public slots:
  virtual void onRecompilation();
  virtual void onPortRenamed(QString path, QString newName);
};

#endif 