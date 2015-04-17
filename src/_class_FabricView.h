#ifndef _FABRICVIEW_H_
#define _FABRICVIEW_H_

#include "_class_ModoTools.h"
#include "_class_FabricDFGWidget.h"

class FabricDFGWidget;
void feLog(const std::string &s);

class FabricView : public CLxImpl_CustomView
{
public:

  LxResult customview_Init(ILxUnknownID pane);

  // to be called within the plugin initialize
  static void initialize();

  //
  FabricView()
  {
    // add pointer to static std::vector.
    s_FabricViews.push_back(this);
  }
  ~FabricView()
  {
    // remove pointer from static std::vector.
    for (int i=0;i<s_FabricViews.size();i++)
      if (s_FabricViews[i] == this)
      {
        s_FabricViews.erase(s_FabricViews.begin() + i);
        break;
      }
  }

  // to be used when constructing the FabricDFGWidget
  QWidget * parentWidget();

  // setter / getter for the contained FabricDFGWidget
  FabricDFGWidget * widget();
  void setWidget(FabricDFGWidget * dfgWidget);

  // static vector of pointers.
  static std::vector <FabricView *> s_FabricViews;

private:

  QWidget * m_parentWidget;
  FabricDFGWidget * m_dfgWidget;
};

#endif _FABRICVIEW_H_