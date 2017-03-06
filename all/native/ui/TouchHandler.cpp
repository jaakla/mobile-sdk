#include "TouchHandler.h"
#include "layers/Layer.h"
#include "renderers/MapRenderer.h"
#include "renderers/components/RayIntersectedElement.h"
#include "renderers/cameraevents/CameraPanEvent.h"
#include "renderers/cameraevents/CameraRotationEvent.h"
#include "renderers/cameraevents/CameraTiltEvent.h"
#include "renderers/cameraevents/CameraZoomEvent.h"
#include "components/Options.h"
#include "graphics/ViewState.h"
#include "ui/MapClickInfo.h"
#include "ui/MapEventListener.h"
#include "ui/workers/ClickHandlerWorker.h"
#include "utils/Const.h"
#include "utils/Log.h"

namespace carto {

    TouchHandler::TouchHandler(const std::shared_ptr<MapRenderer>& mapRenderer, const std::shared_ptr<Options>& options) :
        _gestureMode(SINGLE_POINTER_CLICK_GUESS),
        _prevScreenPos1(0, 0),
        _prevScreenPos2(0, 0),
        _swipe1(0, 0),
        _swipe2(0, 0),
        _pointersDown(0),
        _mapMoving(false),
        _noDualPointerYet(true),
        _mapEventListener(),
        _clickHandlerWorker(std::make_shared<ClickHandlerWorker>(options)),
        _clickHandlerThread(),
        _options(options),
        _mapRenderer(mapRenderer),
        _mapRendererListener(),
        _mutex(),
        _onTouchListeners(),
        _onTouchListenersMutex()
    {
    }
        
    TouchHandler::~TouchHandler() {
    }
        
    void TouchHandler::init() {
        _clickHandlerWorker->setComponents(shared_from_this(), _clickHandlerWorker);
        _clickHandlerThread = std::thread(std::ref(*_clickHandlerWorker));

        _mapRendererListener = std::make_shared<MapRendererListener>(shared_from_this());
        _mapRenderer->registerOnChangeListener(_mapRendererListener);
    }
    
    void TouchHandler::deinit() {
        _mapRenderer->unregisterOnChangeListener(_mapRendererListener);
        _mapRendererListener.reset();
        
        _clickHandlerWorker->stop();
        _clickHandlerThread.detach();
    }
    
    std::shared_ptr<MapEventListener> TouchHandler::getMapEventListener() const {
        return _mapEventListener.get();
    }
    
    void TouchHandler::setMapEventListener(const std::shared_ptr<MapEventListener>& mapEventListener) {
        _mapEventListener.set(mapEventListener);
    }
    
    void TouchHandler::onTouchEvent(int action, const ScreenPos& screenPos1, const ScreenPos& screenPos2) {
        std::vector<std::shared_ptr<OnTouchListener> > onTouchListeners;
        {
            std::lock_guard<std::mutex> lock(_onTouchListenersMutex);
            onTouchListeners = _onTouchListeners;
        }
        for (std::size_t i = onTouchListeners.size(); i-- > 0; ) {
            if (onTouchListeners[i]->onTouchEvent(action, screenPos1, screenPos2)) {
                return;
            }
        }
    
        switch (action) {
        case ACTION_POINTER_1_DOWN:
            if (!_clickHandlerWorker->isRunning()) {
                _clickHandlerWorker->init();
            }
            _clickHandlerWorker->pointer1Down(screenPos1);
            _noDualPointerYet = true;
            _mapRenderer->getKineticEventHandler().stopPan();
            _mapRenderer->getKineticEventHandler().stopRotation();
            _mapRenderer->getKineticEventHandler().stopZoom();
            break;
    
        case ACTION_POINTER_2_DOWN:
            _noDualPointerYet = false;
            switch (_gestureMode) {
            case SINGLE_POINTER_CLICK_GUESS: {
                    std::lock_guard<std::mutex> lock(_mutex);
                    _clickHandlerWorker->pointer2Down(screenPos2);
                    _gestureMode = DUAL_POINTER_CLICK_GUESS;
                }
                break;
            case SINGLE_POINTER_PAN:
            case SINGLE_POINTER_ZOOM:
                startDualPointer(screenPos1, screenPos2);
                break;
            default:
                break;
            }
            break;
    
        case ACTION_MOVE:
            switch (_gestureMode) {
            case SINGLE_POINTER_CLICK_GUESS:
                _clickHandlerWorker->pointer1Moved(screenPos1);
                break;
            case DUAL_POINTER_CLICK_GUESS:
                _clickHandlerWorker->pointer1Moved(screenPos1);
                _clickHandlerWorker->pointer2Moved(screenPos2);
                break;
            case SINGLE_POINTER_PAN:
                {
                    auto deltaTime = std::chrono::steady_clock::now() - _dualPointerReleaseTime;
                    if (deltaTime >= DUAL_STOP_HOLD_DURATION) {
                        singlePointerPan(screenPos1);
                    }
                }
                break;
            case SINGLE_POINTER_ZOOM:
                singlePointerZoom(screenPos1);
                break;
            case DUAL_POINTER_GUESS:
                dualPointerGuess(screenPos1, screenPos2);
                break;
            case DUAL_POINTER_TILT:
                dualPointerTilt(screenPos1);
                break;
            case DUAL_POINTER_ROTATE:
            case DUAL_POINTER_SCALE:
                if (_options->getPanningMode() == PanningMode::PANNING_MODE_STICKY) {
                    float factor = calculateRotatingScalingFactor(screenPos1, screenPos2);
                    if (factor > ROTATION_SCALING_FACTOR_THRESHOLD_STICKY) {
                        _gestureMode = DUAL_POINTER_ROTATE;
                    } else if (factor < -ROTATION_SCALING_FACTOR_THRESHOLD_STICKY) {
                        _gestureMode = DUAL_POINTER_SCALE;
                    }
                }
                dualPointerPan(screenPos1, screenPos2, _gestureMode == DUAL_POINTER_ROTATE, _gestureMode == DUAL_POINTER_SCALE);
                break;
            case DUAL_POINTER_FREE:
                dualPointerPan(screenPos1, screenPos2, true, true);
                break;
            }
            break;
    
        case ACTION_CANCEL: {
                std::lock_guard<std::mutex> lock(_mutex);
                _pointersDown = 0;
                _clickHandlerWorker->cancel();
                _gestureMode = SINGLE_POINTER_CLICK_GUESS;
            }
            break;
        case ACTION_POINTER_1_UP:
            switch (_gestureMode) {
            case SINGLE_POINTER_CLICK_GUESS:
                _clickHandlerWorker->pointer1Up();
                break;
            case DUAL_POINTER_CLICK_GUESS: {
                std::lock_guard<std::mutex> lock(_mutex);
                _clickHandlerWorker->pointer1Up();
                _gestureMode = SINGLE_POINTER_CLICK_GUESS;
                break;
            }
            case SINGLE_POINTER_PAN: {
                {
                    std::lock_guard<std::mutex> lock(_mutex);
                    _gestureMode = SINGLE_POINTER_CLICK_GUESS;
                }
                if (_noDualPointerYet) {
                    _mapRenderer->getKineticEventHandler().startPan();
                } else {
                    auto deltaTime = std::chrono::steady_clock::now() - _dualPointerReleaseTime;
                    if (deltaTime < DUAL_KINETIC_HOLD_DURATION) {
                        _mapRenderer->getKineticEventHandler().startRotation();
                        _mapRenderer->getKineticEventHandler().startZoom();
                    }
                }
                break;
            }
            case SINGLE_POINTER_ZOOM: {
                {
                    std::lock_guard<std::mutex> lock(_mutex);
                    _gestureMode = SINGLE_POINTER_CLICK_GUESS;
                    if (screenPos1 == _prevScreenPos1) {
                        CameraZoomEvent cameraZoomTargetEvent;
                        cameraZoomTargetEvent.setZoomDelta(1.0f);
                        cameraZoomTargetEvent.setTargetPos(_mapRenderer->screenToWorld(screenPos1));
                        _mapRenderer->calculateCameraEvent(cameraZoomTargetEvent, ZOOM_GESTURE_ANIMATION_DURATION.count() / 1000.0f, true);
                    }
                }
                if (_noDualPointerYet) {
                    _mapRenderer->getKineticEventHandler().startZoom();
                }
                break;
            }
            case DUAL_POINTER_GUESS:
            case DUAL_POINTER_TILT:
            case DUAL_POINTER_ROTATE:
            case DUAL_POINTER_SCALE:
            case DUAL_POINTER_FREE: {
                std::lock_guard<std::mutex> lock(_mutex);
                _dualPointerReleaseTime = std::chrono::steady_clock::now();
                _prevScreenPos1 = screenPos2;
                _gestureMode = SINGLE_POINTER_PAN;
                break;
            }
            }
            break;
    
        case ACTION_POINTER_2_UP:
            switch (_gestureMode) {
            case DUAL_POINTER_CLICK_GUESS: {
                std::lock_guard<std::mutex> lock(_mutex);
                _clickHandlerWorker->pointer2Up();
                _gestureMode = SINGLE_POINTER_CLICK_GUESS;
                break;
            }
            case DUAL_POINTER_GUESS:
            case DUAL_POINTER_TILT:
            case DUAL_POINTER_ROTATE:
            case DUAL_POINTER_SCALE:
            case DUAL_POINTER_FREE: {
                std::lock_guard<std::mutex> lock(_mutex);
                _dualPointerReleaseTime = std::chrono::steady_clock::now();
                _prevScreenPos1 = screenPos1;
                _gestureMode = SINGLE_POINTER_PAN;
                break;
            }
            default:
                break;
            }
            break;
        }

        if (action == ACTION_POINTER_1_DOWN || action == ACTION_POINTER_2_DOWN) {
            std::lock_guard<std::mutex> lock(_mutex);
            _pointersDown = std::min(2, _pointersDown + 1);
        } else if (action == ACTION_POINTER_1_UP || action == ACTION_POINTER_2_UP) {
            std::lock_guard<std::mutex> lock(_mutex);
            _pointersDown = std::max(0, _pointersDown - 1);
        }

        if (!_mapRenderer->getKineticEventHandler().isPanning() && !_mapRenderer->getKineticEventHandler().isRotating() && !_mapRenderer->getKineticEventHandler().isZooming()) {
            checkMapStable();
        }
    }
    
    void TouchHandler::checkMapStable() {
        bool stable = false;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_pointersDown == 0 && !_mapMoving) {
                stable = true;
            }
        }

        if (stable) {
            DirectorPtr<MapEventListener> mapEventListener = _mapEventListener;

            if (mapEventListener) {
                mapEventListener->onMapStable();
            }
        }
    }
    
    bool TouchHandler::isValidTouchPosition(const MapPos& mapPos, const ViewState& viewState) const {
        MapVec zVec = (viewState.getFocusPos() - viewState.getCameraPos()).getNormalized();
        double dist = zVec.dotProduct(mapPos - viewState.getCameraPos());
        return dist > 0 && dist < viewState.getFar();
    }

    float TouchHandler::calculateRotatingScalingFactor(const ScreenPos& screenPos1, const ScreenPos& screenPos2) const {
        cglib::vec2<float> prevDelta(_prevScreenPos1.getX() - _prevScreenPos2.getX(), _prevScreenPos1.getY() - _prevScreenPos2.getY());
        cglib::vec2<float> moveDelta(screenPos1.getX() - _prevScreenPos1.getX(), screenPos1.getY() - _prevScreenPos1.getY());
        double factor = 0.0;
        for (int i = 0; i < 2; i++) {
            if (cglib::length(prevDelta) > 0 && cglib::length(moveDelta) > 0) {
                float cos = std::abs(cglib::dot_product(moveDelta, prevDelta)) / cglib::length(moveDelta) / cglib::length(prevDelta);
                float sin = std::sqrt(1.0f - std::min(1.0f, cos * cos));
                float tan = sin / cos;
                factor += std::log(tan); // convert range [0, 1] to range [-inf, 0] and range [1, inf] to range [0, inf]
            }

            moveDelta = cglib::vec2<float>(screenPos2.getX() - _prevScreenPos2.getX(), screenPos2.getY() - _prevScreenPos2.getY());
        }
        return static_cast<float>(factor);
    }
    
    void TouchHandler::singlePointerPan(const ScreenPos& screenPos) {
        if (_options->isUserInput()) {
            _mapRenderer->getAnimationHandler().stopPan();
            _mapRenderer->getAnimationHandler().stopRotation();
            _mapRenderer->getAnimationHandler().stopTilt();
            _mapRenderer->getAnimationHandler().stopZoom();
            
            MapPos currentPos(_mapRenderer->screenToWorld(screenPos));
            MapPos prevPos(_mapRenderer->screenToWorld(_prevScreenPos1));
            
            ViewState viewState = _mapRenderer->getViewState();
            if (isValidTouchPosition(currentPos, viewState) && isValidTouchPosition(prevPos, viewState)) {
                CameraPanEvent cameraEvent;
                cameraEvent.setPosDelta(prevPos - currentPos);
                _mapRenderer->calculateCameraEvent(cameraEvent, 0, true);
            }
        }
        _prevScreenPos1 = screenPos;
    }
    
    void TouchHandler::singlePointerZoom(const ScreenPos& screenPos) {
        if (_options->isUserInput()) {
            _mapRenderer->getAnimationHandler().stopPan();
            _mapRenderer->getAnimationHandler().stopRotation();
            _mapRenderer->getAnimationHandler().stopTilt();
            _mapRenderer->getAnimationHandler().stopZoom();
            
            float delta = INCHES_TO_ZOOM_DELTA * (screenPos.getY() - _prevScreenPos1.getY()) / _options->getDPI();
            CameraZoomEvent cameraEvent;
            cameraEvent.setZoomDelta(delta);
            _mapRenderer->calculateCameraEvent(cameraEvent, 0, true);
        }
        _prevScreenPos1 = screenPos;
    }
    
    void TouchHandler::dualPointerGuess(const ScreenPos& screenPos1, const ScreenPos& screenPos2) {
        // If the pointers' y coordinates differ too much it's the general case or rotation
        float dpi = _options->getDPI();
        float deltaY = std::abs(screenPos1.getY() - screenPos2.getY()) / dpi;
        if (deltaY > GUESS_MAX_DELTA_Y_INCHES) {
            _gestureMode = DUAL_POINTER_FREE;
        } else {
    
            // Calculate swipe vectors
            cglib::vec2<float> tempSwipe1(screenPos1.getX() - _prevScreenPos1.getX(), screenPos1.getY() - _prevScreenPos1.getY());
            _swipe1 += tempSwipe1 * (1.0f / dpi);
            cglib::vec2<float> tempSwipe2(screenPos2.getX() - _prevScreenPos2.getX(), screenPos2.getY() - _prevScreenPos2.getY());
            _swipe2 += tempSwipe2 * (1.0f / dpi);
            
            float swipe1Length = cglib::length(_swipe1);
            float swipe2Length = cglib::length(_swipe2);
    
            // Check if swipes have opposite directions or same directions
            if ((swipe1Length > GUESS_MIN_SWIPE_LENGTH_OPPOSITE_INCHES ||
                    swipe2Length > GUESS_MIN_SWIPE_LENGTH_OPPOSITE_INCHES)
                    && _swipe1(1) * _swipe2(1) <= 0) {
                _gestureMode = DUAL_POINTER_FREE;
            } else if (swipe1Length > GUESS_MIN_SWIPE_LENGTH_SAME_INCHES ||
                    swipe2Length > GUESS_MIN_SWIPE_LENGTH_SAME_INCHES) {
                // Check if the angle of the same direction swipes
                if (std::abs(_swipe1(0) / swipe1Length) > GUESS_SWIPE_ABS_COS_THRESHOLD ||
                    std::abs(_swipe2(0) / swipe2Length) > GUESS_SWIPE_ABS_COS_THRESHOLD) {
                    _gestureMode = DUAL_POINTER_FREE;
                } else {
                    _gestureMode = DUAL_POINTER_TILT;
                }
            }
        }
    
        // Detect rotation/scaling gesture if general panning mode is switched off
        if (_gestureMode == DUAL_POINTER_FREE && _options->getPanningMode() != PanningMode::PANNING_MODE_FREE) {
            float factor = calculateRotatingScalingFactor(screenPos1, screenPos2);
            if (factor > ROTATION_FACTOR_THRESHOLD) {
                _gestureMode = DUAL_POINTER_ROTATE;
            } else if (factor < -SCALING_FACTOR_THRESHOLD) {
                _gestureMode = DUAL_POINTER_SCALE;
            } else {
                _gestureMode = DUAL_POINTER_GUESS;
                return;
            }
        }
    
        // The general case requires _previous coordinates for both pointers,
        // calculate them
        switch (_gestureMode) {
        case DUAL_POINTER_ROTATE:
        case DUAL_POINTER_SCALE:
        case DUAL_POINTER_FREE:
            _prevScreenPos1 = screenPos1;
            _prevScreenPos2 = screenPos2;
            break;
        case DUAL_POINTER_GUESS:
        case DUAL_POINTER_TILT:
        default:
            _prevScreenPos1 = screenPos1;
            _prevScreenPos2 = screenPos2;
        }
    }
    
    void TouchHandler::dualPointerTilt(const ScreenPos& screenPos) {
        if (_options->isUserInput()) {
            _mapRenderer->getAnimationHandler().stopPan();
            _mapRenderer->getAnimationHandler().stopRotation();
            _mapRenderer->getAnimationHandler().stopTilt();
            _mapRenderer->getAnimationHandler().stopZoom();
            
            float scale = INCHES_TO_TILT_DELTA / _options->getDPI();
            if (_options->isTiltGestureReversed()) {
                scale = -scale;
            }
            CameraTiltEvent cameraEvent;
            cameraEvent.setTiltDelta((screenPos.getY() - _prevScreenPos1.getY()) * scale);
            _mapRenderer->calculateCameraEvent(cameraEvent, 0, false);
        }
        _prevScreenPos1 = screenPos;
    }
    
    void TouchHandler::dualPointerPan(const ScreenPos& screenPos1, const ScreenPos& screenPos2, bool rotate, bool scale) {
        if (_options->isUserInput()) {
            _mapRenderer->getAnimationHandler().stopPan();
            _mapRenderer->getAnimationHandler().stopRotation();
            _mapRenderer->getAnimationHandler().stopTilt();
            _mapRenderer->getAnimationHandler().stopZoom();
            
            MapPos currentPos1(_mapRenderer->screenToWorld(screenPos1));
            MapPos currentPos2(_mapRenderer->screenToWorld(screenPos2));
            MapPos prevPos1(_mapRenderer->screenToWorld(_prevScreenPos1));
            MapPos prevPos2(_mapRenderer->screenToWorld(_prevScreenPos2));
            MapVec currentVec(currentPos2 - currentPos1);
            MapVec prevVec(prevPos2 - prevPos1);
            MapPos currentMiddlePos = currentPos1 + currentVec * 0.5;
            MapPos prevMiddlePos = prevPos1 + prevVec * 0.5;
            MapPos pivotPos = (_options->getPivotMode() == PivotMode::PIVOT_MODE_TOUCHPOINT ? currentMiddlePos : _mapRenderer->getFocusPos());

            ViewState viewState = _mapRenderer->getViewState();
            if (isValidTouchPosition(currentPos1, viewState) && isValidTouchPosition(prevPos1, viewState)
            &&  isValidTouchPosition(currentPos2, viewState) && isValidTouchPosition(prevPos2, viewState)) {
                if (_options->getPivotMode() == PivotMode::PIVOT_MODE_TOUCHPOINT) {
                    CameraPanEvent cameraPanEvent;
                    cameraPanEvent.setPosDelta(prevMiddlePos - currentMiddlePos);
                    _mapRenderer->calculateCameraEvent(cameraPanEvent, 0, true);
                }
            
                if (scale && prevVec.length() > 0 && currentVec.length() > 0) {
                    float ratio = static_cast<float>(prevVec.length() / currentVec.length());
                    CameraZoomEvent cameraZoomTargetEvent;
                    cameraZoomTargetEvent.setScale(ratio);
                    cameraZoomTargetEvent.setTargetPos(pivotPos);
                    _mapRenderer->calculateCameraEvent(cameraZoomTargetEvent, 0, true);
                }
    
                if (rotate && prevVec.length() > 0 && currentVec.length() > 0) {
                    currentVec.normalize();
                    prevVec.normalize();
                    double sin = currentVec.crossProduct2D(prevVec);
                    double cos = currentVec.dotProduct(prevVec);
                    CameraRotationEvent cameraRotateTargetEvent;
                    cameraRotateTargetEvent.setRotationDelta(sin, cos);
                    cameraRotateTargetEvent.setTargetPos(pivotPos);
                    _mapRenderer->calculateCameraEvent(cameraRotateTargetEvent, 0, true);
                }
            }
        }
    
        _prevScreenPos1 = screenPos1;
        _prevScreenPos2 = screenPos2;
    }
    
    void TouchHandler::click(const ScreenPos& screenPos) {
        if (!_options->isUserInput()) {
            return;
        }
        
        _mapRenderer->getAnimationHandler().stopPan();
        _mapRenderer->getAnimationHandler().stopRotation();
        _mapRenderer->getAnimationHandler().stopTilt();
        _mapRenderer->getAnimationHandler().stopZoom();
        
        handleClick(ClickType::CLICK_TYPE_SINGLE, _mapRenderer->screenToWorld(screenPos));
    }
    
    
    void TouchHandler::longClick(const ScreenPos& screenPos) {
        startSinglePointer(screenPos);
        
        if (!_options->isUserInput()) {
            return;
        }
        
        _mapRenderer->getAnimationHandler().stopPan();
        _mapRenderer->getAnimationHandler().stopRotation();
        _mapRenderer->getAnimationHandler().stopTilt();
        _mapRenderer->getAnimationHandler().stopZoom();
    
        handleClick(ClickType::CLICK_TYPE_LONG, _mapRenderer->screenToWorld(screenPos));
    }
    
    void TouchHandler::doubleClick(const ScreenPos& screenPos) {
        if (!_options->isUserInput()) {
            return;
        }
        
        _mapRenderer->getAnimationHandler().stopPan();
        _mapRenderer->getAnimationHandler().stopRotation();
        _mapRenderer->getAnimationHandler().stopTilt();
        _mapRenderer->getAnimationHandler().stopZoom();

        if (_options->isZoomGestures()) {
            _prevScreenPos1 = screenPos;
            _gestureMode = SINGLE_POINTER_ZOOM;
        } else {
            handleClick(ClickType::CLICK_TYPE_DOUBLE, _mapRenderer->screenToWorld(screenPos));
        }
    }
    
    void TouchHandler::dualClick(const ScreenPos& screenPos1, const ScreenPos& screenPos2) {
        if (!_options->isUserInput()) {
            return;
        }
        
        _mapRenderer->getAnimationHandler().stopPan();
        _mapRenderer->getAnimationHandler().stopRotation();
        _mapRenderer->getAnimationHandler().stopTilt();
        _mapRenderer->getAnimationHandler().stopZoom();

        if (_options->isZoomGestures()) {
            CameraZoomEvent cameraZoomTargetEvent;
            cameraZoomTargetEvent.setZoomDelta(-1.0f);
            cameraZoomTargetEvent.setTargetPos(_mapRenderer->getFocusPos());
            _mapRenderer->calculateCameraEvent(cameraZoomTargetEvent, ZOOM_GESTURE_ANIMATION_DURATION.count() / 1000.0f, true);
        } else {
            ScreenPos centreScreenPos((screenPos1.getX() + screenPos2.getX()) / 2, (screenPos1.getY() + screenPos2.getY()) / 2);
            handleClick(ClickType::CLICK_TYPE_DUAL, _mapRenderer->screenToWorld(centreScreenPos));
        }
    }
    
    void TouchHandler::handleClick(ClickType::ClickType clickType, const MapPos& targetPos) {
        ViewState viewState;
        std::vector<RayIntersectedElement> results;
        _mapRenderer->calculateRayIntersectedElements(targetPos, viewState, results);

        for (const RayIntersectedElement& intersectedElement : results) {
            if (intersectedElement.getLayer()->processClick(clickType, intersectedElement, viewState)) {
                return;
            }
        }

        DirectorPtr<MapEventListener> mapEventListener = _mapEventListener;

        if (mapEventListener) {
            ViewState viewState = _mapRenderer->getViewState();
            if (isValidTouchPosition(targetPos, viewState)) {
                mapEventListener->onMapClicked(std::make_shared<MapClickInfo>(clickType, _options->getBaseProjection()->fromInternal(targetPos)));
            }
        }
    }

    void TouchHandler::startSinglePointer(const ScreenPos& screenPos) {
        std::lock_guard<std::mutex> lock(_mutex);
        _prevScreenPos1 = screenPos;
        _gestureMode = SINGLE_POINTER_PAN;
    }
    
    void TouchHandler::startDualPointer(const ScreenPos& screenPos1, const ScreenPos& screenPos2) {
        std::lock_guard<std::mutex> lock(_mutex);
        _swipe1 = cglib::vec2<float>(0, 0);
        _swipe2 = cglib::vec2<float>(0, 0);
        _prevScreenPos1 = screenPos1;
        _prevScreenPos2 = screenPos2;
        _gestureMode = DUAL_POINTER_GUESS;
    }

    void TouchHandler::registerOnTouchListener(const std::shared_ptr<OnTouchListener>& listener) {
        {
            std::lock_guard<std::mutex> lock(_onTouchListenersMutex);
            _onTouchListeners.push_back(listener);
        }
    }

    void TouchHandler::unregisterOnTouchListener(const std::shared_ptr<OnTouchListener>& listener) {
        {
            std::lock_guard<std::mutex> lock(_onTouchListenersMutex);
            _onTouchListeners.erase(std::remove(_onTouchListeners.begin(), _onTouchListeners.end(), listener), _onTouchListeners.end());
        }
    }
    
    TouchHandler::MapRendererListener::MapRendererListener(const std::shared_ptr<TouchHandler>& touchHandler) : _touchHandler(touchHandler) {
    }
    
    void TouchHandler::MapRendererListener::onMapChanged() {
        if (auto touchHandler = _touchHandler.lock()) {
            {
                std::lock_guard<std::mutex> lock(touchHandler->_mutex);
                touchHandler->_mapMoving = true;
            }

            DirectorPtr<MapEventListener> mapEventListener = touchHandler->_mapEventListener;

            if (mapEventListener) {
                mapEventListener->onMapMoved();
            }
        }
    }
    
    void TouchHandler::MapRendererListener::onMapIdle() {
        if (auto touchHandler = _touchHandler.lock()) {
            {
                std::lock_guard<std::mutex> lock(touchHandler->_mutex);
                touchHandler->_mapMoving = false;
            }

            DirectorPtr<MapEventListener> mapEventListener = touchHandler->_mapEventListener;

            if (mapEventListener) {
                mapEventListener->onMapIdle();
            }
            touchHandler->checkMapStable();
        }
    }
    
    const float TouchHandler::GUESS_MAX_DELTA_Y_INCHES = 2.5f;
    const float TouchHandler::GUESS_MIN_SWIPE_LENGTH_SAME_INCHES = 0.2f;
    const float TouchHandler::GUESS_MIN_SWIPE_LENGTH_OPPOSITE_INCHES = 0.06f;
    
    const float TouchHandler::GUESS_SWIPE_ABS_COS_THRESHOLD = 0.707f;
    
    const float TouchHandler::SCALING_FACTOR_THRESHOLD = 0.5f;
    const float TouchHandler::ROTATION_FACTOR_THRESHOLD = 0.75f; // make rotation harder to trigger compared to scaling
    const float TouchHandler::ROTATION_SCALING_FACTOR_THRESHOLD_STICKY = 3.0f;
    
    const float TouchHandler::INCHES_TO_TILT_DELTA = 32.0f;

    const float TouchHandler::INCHES_TO_ZOOM_DELTA = 1.0f;
        
    const std::chrono::milliseconds TouchHandler::DUAL_KINETIC_HOLD_DURATION = std::chrono::milliseconds(100);

    const std::chrono::milliseconds TouchHandler::DUAL_STOP_HOLD_DURATION = std::chrono::milliseconds(75);

    const std::chrono::milliseconds TouchHandler::ZOOM_GESTURE_ANIMATION_DURATION = std::chrono::milliseconds(250);

}
