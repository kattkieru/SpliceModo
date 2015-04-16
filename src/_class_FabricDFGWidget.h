
#ifndef _FABRICDFGWIDGET_H_
#define _FABRICDFGWIDGET_H_

// includes.
#include <QtGui/QWidget>
#include <QtGui/QFileDialog>
#include <QtGui/QSplitter>
#include <QtGui/QDockWidget>

#include <DFGWrapper/DFGWrapper.h>
#include <ASTWrapper/KLASTManager.h>
#include <Commands/CommandStack.h>
#include <map>

#include <FabricUI/DFG/DFGUI.h>
#include <FabricUI/DFG/DFGLogWidget.h>
#include <FabricUI/DFG/DFGCombinedWidget.h>
#include <FabricUI/DFG/DFGValueEditor.h>

#include <FabricSplice.h>

#include "_class_BaseInterface.h"

using namespace FabricServices;
using namespace FabricUI;

class FabricDFGWidget : public DFG::DFGCombinedWidget {

  Q_OBJECT
  
public:
  FabricDFGWidget(QWidget *parent, BaseInterface *baseInterface);
  ~FabricDFGWidget();

  static QDockWidget *getWidgetforBaseInterface(BaseInterface *baseInterface, bool createNewIfNoneFound = true);

public slots:
  virtual void onRecompilation();
  virtual void onPortRenamed(QString path, QString newName);

private:
    static std::map<BaseInterface*, QDockWidget*> s_instances;

public:
    // returns the pointer at the main Qt window.
    static QMainWindow *GetPointerAtMainWindow(void);
};

#endif 
