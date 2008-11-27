/*
 * Stellarium
 * Copyright (C) 2007 Fabien Chereau
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include <config.h>

#include "StelApp.hpp"
#include "StelCore.hpp"
#include "StelFileMgr.hpp"
#include "StelProjector.hpp"
#include "fixx11h.h"
#include "StelAppGraphicsScene.hpp"
#include "ViewportDistorter.hpp"
#include "StelModuleMgr.hpp"
#include "StelMainWindow.hpp"
#include "StelMainGraphicsView.hpp"

#include <QtOpenGL>
#include <QTimer>

StelAppGraphicsScene* StelAppGraphicsScene::singleton = NULL;
 
StelAppGraphicsScene::StelAppGraphicsScene() : tempPainter(NULL), cursorTimeout(-1.f), flagCursorTimeout(false), minFpsTimer(NULL)
{
	Q_ASSERT(!singleton);
	singleton = this;
	
	distorter = ViewportDistorter::create("none",800,600,StelProjectorP());
	lastEventTimeSec = StelApp::getTotalRunTime();
	previousTime = lastEventTimeSec;
}

StelAppGraphicsScene::~StelAppGraphicsScene()
{
	if (distorter)
	{
		delete distorter;
		distorter=NULL;
	}
}

void StelAppGraphicsScene::init()
{
	QSettings* conf = StelApp::getInstance().getSettings();
	Q_ASSERT(conf);
	setViewPortDistorterType(conf->value("video/distorter","none").toString());
	setFlagCursorTimeout(conf->value("gui/flag_mouse_cursor_timeout", false).toBool());
	setCursorTimeout(conf->value("gui/mouse_cursor_timeout", 10.).toDouble());

	connect(&StelApp::getInstance(), SIGNAL(minFpsChanged()), this, SLOT(minFpsChanged()));
	
	startMainLoop();
}

void StelAppGraphicsScene::glWindowHasBeenResized(int w, int h)
{
	if (!distorter || (distorter && distorter->getType() == "none"))
	{
		StelApp::getInstance().glWindowHasBeenResized(w, h);
	}
}

//! Switch to native OpenGL painting, i.e not using QPainter
//! After this call revertToQtPainting MUST be called
void StelAppGraphicsScene::switchToNativeOpenGLPainting()
{
	// Save openGL projection state
	glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
	glPushAttrib(GL_ALL_ATTRIB_BITS);
	glMatrixMode(GL_TEXTURE);
	glPushMatrix();
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glShadeModel(GL_FLAT);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_LIGHTING);
	//glDisable(GL_MULTISAMPLE);
	glDisable(GL_DITHER);
	glDisable(GL_ALPHA_TEST);
}

//! Revert openGL state so that Qt painting works again
//! @return a painter that can be used
QPainter* StelAppGraphicsScene::revertToQtPainting()
{
	// Restore openGL projection state for Qt drawings
	glMatrixMode(GL_TEXTURE);
	glPopMatrix();
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();
	glPopAttrib();
	glPopClientAttrib();
	return tempPainter;
}


QPainter* StelAppGraphicsScene::switchToQPainting()
{
	// Save openGL projection state
	glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
	glPushAttrib(GL_ALL_ATTRIB_BITS);
	glMatrixMode(GL_TEXTURE);
	glPushMatrix();
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	
	if (tempPainter)
	{
		if (tempPainter->isActive())
		{
			tempPainter->save();
			tempPainter->end();
			tempPainter->begin(StelAppGraphicsScene::getInstance().views().at(0));
			tempPainter->resetTransform();
		}
		else
		{
			tempPainter->begin(StelAppGraphicsScene::getInstance().views().at(0));
		}
	}
	return tempPainter;
}

//! Revert openGL state
void StelAppGraphicsScene::revertToOpenGL()
{
	// Restore openGL projection state for Qt drawings
	glMatrixMode(GL_TEXTURE);
	glPopMatrix();
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();
	glPopAttrib();
	glPopClientAttrib();
	
	if (tempPainter)
		tempPainter->restore();
}

void StelAppGraphicsScene::drawBackground(QPainter *painter, const QRectF &)
{
	if (painter->paintEngine()->type() != QPaintEngine::OpenGL)
	{
		qWarning("StelAppGraphicsScene: drawBackground needs a QGLWidget to be set as viewport on the graphics view");
		return;
	}

	const double now = StelApp::getTotalRunTime();
	double dt = now-previousTime;
	previousTime = now;
	if (dt<0)	// This fix the star scale bug!!
		return;
	
	// Update the core and all modules
	StelApp::getInstance().update(dt);
	
	tempPainter = painter;
	switchToNativeOpenGLPainting();
	StelApp::getInstance().glWindowHasBeenResized((int)(width()), (int)(height()));
	
	// And draw them
	distorter->prepare();
	StelApp::getInstance().draw();
	distorter->distort();
	
	revertToQtPainting();
	tempPainter = NULL;
	
	// Determines when the next display will need to be triggered
	// The current policy is that after an event, the FPS is maximum for 2.5 seconds
	// after that, it switches back to the default minfps value to save power
	if (now-lastEventTimeSec<2.5)
	{
		double duration = 1./StelApp::getInstance().maxfps;
		int dur = (int)(duration*1000);
		QTimer::singleShot(dur<5 ? 5 : dur, this, SLOT(update()));
	}
	// Manage cursor timeout
	if (cursorTimeout>0.f && (now-lastEventTimeSec>cursorTimeout) && flagCursorTimeout)
	{
		if (QApplication::overrideCursor()==0)
			QApplication::setOverrideCursor(Qt::BlankCursor);
	}
	else
	{
		if (QApplication::overrideCursor()!=0)
			QApplication::restoreOverrideCursor();
	}
}

void StelAppGraphicsScene::thereWasAnEvent()
{
	lastEventTimeSec = StelApp::getTotalRunTime();
}

void StelAppGraphicsScene::setViewPortDistorterType(const QString &type)
{
	if (type != getViewPortDistorterType())
	{
		if (type == "none")
		{
			StelMainGraphicsView::getInstance().getOpenGLWin()->setMaximumSize(10000,10000);
		}
		else
		{
			StelMainGraphicsView::getInstance().getOpenGLWin()->setFixedSize((int)(width()), (int)(height()));
		}
	}
	if (distorter)
	{
		delete distorter;
		distorter = NULL;
	}
	distorter = ViewportDistorter::create(type,(int)width(),(int)height(),StelApp::getInstance().getCore()->getProjection2d());
}

QString StelAppGraphicsScene::getViewPortDistorterType() const
{
	if (distorter)
		return distorter->getType();
	return "none";
}

void StelAppGraphicsScene::mouseMoveEvent(QGraphicsSceneMouseEvent *event)
{
	thereWasAnEvent(); // Refresh screen ASAP
	
	if (mouseGrabberItem())
	{
		QGraphicsScene::mouseMoveEvent(event);
		if (event->isAccepted())
			return;
	}
	else
		QGraphicsScene::mouseMoveEvent(event);
	
	int x = (int)event->scenePos().x();
	int y = (int)event->scenePos().y();
	y = (int)height() - 1 - y;
	distorter->distortXY(x,y);
	StelApp::getInstance().handleMove(x, y, event->buttons());
	event->accept();
}

void StelAppGraphicsScene::mousePressEvent(QGraphicsSceneMouseEvent* event)
{
	QGraphicsScene::mousePressEvent(event);
	thereWasAnEvent(); // Refresh screen ASAP
	StelMainGraphicsView::getInstance().activateKeyActions(!(hasFocus() && focusItem()!=0));
	if (event->isAccepted())
		return;
	
	if (activeWindow()!=0 && activeWindow()->isUnderMouse())
	{
		event->accept();
		return;
	}
	
	int x = (int)event->scenePos().x();
	int y = (int)event->scenePos().y();
	y = (int)height() - 1 - y;
	distorter->distortXY(x,y);
	QMouseEvent newEvent(QEvent::MouseButtonPress, QPoint(x,y), event->button(), event->buttons(), event->modifiers());
	StelApp::getInstance().handleClick(&newEvent);
	event->accept();
}

void StelAppGraphicsScene::mouseReleaseEvent(QGraphicsSceneMouseEvent* event)
{
	QGraphicsScene::mouseReleaseEvent(event);
	thereWasAnEvent(); // Refresh screen ASAP
	StelMainGraphicsView::getInstance().activateKeyActions(!(hasFocus() && focusItem()!=0));
	if (event->isAccepted())
		return;

	if (activeWindow()!=0 && activeWindow()->isUnderMouse())
	{
		event->accept();
		return;
	}
	
	int x = (int)event->scenePos().x();
	int y = (int)event->scenePos().y();
	y = (int)height() - 1 - y;
	distorter->distortXY(x,y);
	QMouseEvent newEvent(QEvent::MouseButtonRelease, QPoint(x,y), event->button(), event->buttons(), event->modifiers());
	StelApp::getInstance().handleClick(&newEvent);
	event->accept();
}

void StelAppGraphicsScene::wheelEvent(QGraphicsSceneWheelEvent* event)
{
	QGraphicsScene::wheelEvent(event);
	thereWasAnEvent(); // Refresh screen ASAP
	if (event->isAccepted())
		return;
	
	if (activeWindow()!=0 && activeWindow()->isUnderMouse())
	{
		event->accept();
		return;
	}
	
	int x = (int)event->pos().x();
	int y = (int)event->pos().y();
	y = (int)height() - 1 - y;
	distorter->distortXY(x,y);
	QWheelEvent newEvent(QPoint(x,y), event->delta(), event->buttons(), event->modifiers(), event->orientation());
	StelApp::getInstance().handleWheel(&newEvent);
	event->accept();
}

void StelAppGraphicsScene::keyPressEvent(QKeyEvent* event)
{
	thereWasAnEvent(); // Refresh screen ASAP
	QGraphicsScene::keyPressEvent(event);
	StelMainGraphicsView::getInstance().activateKeyActions(!(hasFocus() && focusItem()!=0));
	if (event->isAccepted())
		return;
	
	if (activeWindow()!=0)
	{
		event->accept();
		return;
	}
	
	StelApp::getInstance().handleKeys(event);
}

void StelAppGraphicsScene::keyReleaseEvent(QKeyEvent* event)
{
	thereWasAnEvent(); // Refresh screen ASAP
	QGraphicsScene::keyReleaseEvent(event);
	StelMainGraphicsView::getInstance().activateKeyActions(!(hasFocus() && focusItem()!=0));
	if (event->isAccepted())
		return;
	
	if (activeWindow()!=0)
	{
		event->accept();
		return;
	}
	
	StelApp::getInstance().handleKeys(event);
}

void StelAppGraphicsScene::startMainLoop()
{
	// Set a timer refreshing for every minfps frames
	minFpsChanged();
}

void StelAppGraphicsScene::minFpsChanged()
{
	if (minFpsTimer!=NULL)
	{
		disconnect(minFpsTimer, SIGNAL(timeout()), 0, 0);
		delete minFpsTimer;
		minFpsTimer = NULL;
	}
		
	minFpsTimer = new QTimer(this);
	connect(minFpsTimer, SIGNAL(timeout()), this, SLOT(update()));
	minFpsTimer->start((int)(1./StelApp::getInstance().minfps*1000.));
}
