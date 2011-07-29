//
// Urho3D Engine
// Copyright (c) 2008-2011 Lasse ��rni
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "Precompiled.h"
#include "Component.h"
#include "Context.h"
#include "CoreEvents.h"
#include "File.h"
#include "Log.h"
#include "PackageFile.h"
#include "Profiler.h"
#include "Scene.h"
#include "SceneEvents.h"
#include "XMLFile.h"

static const int ASYNC_LOAD_MIN_FPS = 50;
static const int ASYNC_LOAD_MAX_MSEC = (int)(1000.0f / ASYNC_LOAD_MIN_FPS);
static const float DEFAULT_SMOOTHING_CONSTANT = 50.0f;
static const float DEFAULT_SNAP_THRESHOLD = 1.0f;

OBJECTTYPESTATIC(Scene);

Scene::Scene(Context* context) :
    Node(context),
    replicatedNodeID_(FIRST_REPLICATED_ID),
    replicatedComponentID_(FIRST_REPLICATED_ID),
    localNodeID_(FIRST_LOCAL_ID),
    localComponentID_(FIRST_LOCAL_ID),
    smoothingConstant_(DEFAULT_SMOOTHING_CONSTANT),
    snapThreshold_(DEFAULT_SNAP_THRESHOLD),
    checksum_(0),
    active_(true),
    asyncLoading_(false)
{
    // Assign an ID to self so that nodes can refer to this node as a parent
    SetID(GetFreeNodeID(REPLICATED));
    NodeAdded(this);
    
    SubscribeToEvent(E_UPDATE, HANDLER(Scene, HandleUpdate));
}

Scene::~Scene()
{
    // Remove scene reference and owner from all nodes that still exist
    for (Map<unsigned, Node*>::Iterator i = allNodes_.Begin(); i != allNodes_.End(); ++i)
    {
        i->second_->SetScene(0);
        i->second_->SetOwner(0);
    }
}

void Scene::RegisterObject(Context* context)
{
    context->RegisterFactory<Scene>();
    context->CopyBaseAttributes<Node, Scene>();
    
    ATTRIBUTE(Scene, VAR_INT, "Next Replicated Node ID", replicatedNodeID_, FIRST_REPLICATED_ID, AM_DEFAULT);
    ATTRIBUTE(Scene, VAR_INT, "Next Replicated Component ID", replicatedComponentID_, FIRST_REPLICATED_ID, AM_DEFAULT);
    ATTRIBUTE(Scene, VAR_INT, "Next Local Node ID", localNodeID_, FIRST_LOCAL_ID, AM_DEFAULT);
    ATTRIBUTE(Scene, VAR_INT, "Next Local Component ID", localComponentID_, FIRST_LOCAL_ID, AM_DEFAULT);
    ATTRIBUTE(Scene, VAR_FLOAT, "Motion Smoothing Constant", smoothingConstant_, DEFAULT_SMOOTHING_CONSTANT, AM_DEFAULT);
    ATTRIBUTE(Scene, VAR_FLOAT, "Motion Snap Threshold", snapThreshold_, DEFAULT_SNAP_THRESHOLD, AM_DEFAULT);
}

bool Scene::Load(Deserializer& source)
{
    StopAsyncLoading();
    
    // Check ID
    if (source.ReadID() != "USCN")
    {
        LOGERROR(source.GetName() + " is not a valid scene file");
        return false;
    }
    
    LOGINFO("Loading scene from " + source.GetName());
    
    // Load the whole scene, then perform post-load if successfully loaded
    if (Node::Load(source))
    {
        FinishLoading(&source);
        return true;
    }
    else
        return false;
}

bool Scene::Save(Serializer& dest)
{
    // Write ID first
    if (!dest.WriteID("USCN"))
    {
        LOGERROR("Could not save scene, writing to stream failed");
        return false;
    }
    
    Deserializer* ptr = dynamic_cast<Deserializer*>(&dest);
    if (ptr)
        LOGINFO("Saving scene to " + ptr->GetName());
    
    return Node::Save(dest);
}

bool Scene::LoadXML(const XMLElement& source)
{
    StopAsyncLoading();
    
    // Load the whole scene, then perform post-load if successfully loaded
    // Note: the scene filename and checksum can not be set, as we only used an XML element
    if (Node::LoadXML(source))
    {
        FinishLoading(0);
        return true;
    }
    else
        return false;
}

bool Scene::LoadXML(Deserializer& source)
{
    StopAsyncLoading();
    
    SharedPtr<XMLFile> xml(new XMLFile(context_));
    if (!xml->Load(source))
        return false;
    
    LOGINFO("Loading scene from " + source.GetName());
    
    // Load the whole scene, then perform post-load if successfully loaded
    if (Node::LoadXML(xml->GetRoot()))
    {
        FinishLoading(&source);
        return true;
    }
    else
        return false;
}

bool Scene::SaveXML(Serializer& dest)
{
    SharedPtr<XMLFile> xml(new XMLFile(context_));
    XMLElement rootElem = xml->CreateRoot("scene");
    if (!SaveXML(rootElem))
        return false;
    
    Deserializer* ptr = dynamic_cast<Deserializer*>(&dest);
    if (ptr)
        LOGINFO("Saving scene to " + ptr->GetName());
    
    return xml->Save(dest);
}

bool Scene::LoadAsync(File* file)
{
    if (!file)
    {
        LOGERROR("Null file for async loading");
        return false;
    }
    
    StopAsyncLoading();
    
    // Check ID
    if (file->ReadID() != "USCN")
    {
        LOGERROR(file->GetName() + " is not a valid scene file");
        return false;
    }
    
    LOGINFO("Loading scene from " + file->GetName());
    
    // Clear the previous scene and load the root level components first
    Clear();
    if (!Node::Load(*file, false))
        return false;
    
    // Then prepare for loading all root level child nodes in the async update
    asyncLoading_ = true;
    asyncProgress_.file_ = file;
    asyncProgress_.xmlFile_.Reset();
    asyncProgress_.xmlElement_ = XMLElement();
    asyncProgress_.loadedNodes_ = 0;
    asyncProgress_.totalNodes_ = file->ReadVLE();
    
    return true;
}

bool Scene::LoadAsyncXML(File* file)
{
    if (!file)
    {
        LOGERROR("Null file for async loading");
        return false;
    }
    
    StopAsyncLoading();
    
    SharedPtr<XMLFile> xmlFile(new XMLFile(context_));
    if (!xmlFile->Load(*file))
        return false;
    
    LOGINFO("Loading scene from " + file->GetName());
    
    // Clear the previous scene and load the root level components first
    Clear();
    XMLElement rootElement = xmlFile->GetRoot();
    if (!Node::LoadXML(rootElement, false))
        return false;
    
    // Then prepare for loading all root level child nodes in the async update
    XMLElement childNodeElement = rootElement.GetChild("node");
    asyncLoading_ = true;
    asyncProgress_.file_ = file;
    asyncProgress_.xmlFile_ = xmlFile;
    asyncProgress_.xmlElement_ = childNodeElement;
    asyncProgress_.loadedNodes_ = 0;
    asyncProgress_.totalNodes_ = 0;
    
    // Count the amount of child nodes
    while (childNodeElement)
    {
        ++asyncProgress_.totalNodes_;
        childNodeElement = childNodeElement.GetNext("node");
    }
    
    return true;
}

void Scene::StopAsyncLoading()
{
    asyncLoading_ = false;
    asyncProgress_.file_.Reset();
    asyncProgress_.xmlFile_.Reset();
    asyncProgress_.xmlElement_ = XMLElement();
}

void Scene::Clear()
{
    StopAsyncLoading();
    RemoveAllChildren();
    RemoveAllComponents();
    fileName_ = String();
    checksum_ = 0;
}

void Scene::SetActive(bool enable)
{
    active_ = enable;
}

void Scene::SetSmoothingConstant(float constant)
{
    smoothingConstant_ = Max(constant, M_EPSILON);
}

void Scene::SetSnapThreshold(float threshold)
{
    snapThreshold_ = Max(threshold, 0.0f);
}

void Scene::AddRequiredPackageFile(PackageFile* package)
{
    // Do not add packages that failed to load
    if (!package || !package->GetNumFiles())
        return;
    
    requiredPackageFiles_.Push(SharedPtr<PackageFile>(package));
}

void Scene::ClearRequiredPackageFiles()
{
    requiredPackageFiles_.Clear();
}

void Scene::ResetOwner(Connection* owner)
{
    for (Map<unsigned, Node*>::Iterator i = allNodes_.Begin(); i != allNodes_.End(); ++i)
    {
        if (i->second_->GetOwner() == owner)
            i->second_->SetOwner(0);
    }
}

Node* Scene::GetNodeByID(unsigned id) const
{
    Map<unsigned, Node*>::ConstIterator i = allNodes_.Find(id);
    if (i != allNodes_.End())
        return i->second_;
    else
        return 0;
}

Component* Scene::GetComponentByID(unsigned id) const
{
    Map<unsigned, Component*>::ConstIterator i = allComponents_.Find(id);
    if (i != allComponents_.End())
        return i->second_;
    else
        return 0;
}

float Scene::GetAsyncProgress() const
{
    if (!asyncLoading_ || !asyncProgress_.totalNodes_)
        return 1.0f;
    else
        return (float)asyncProgress_.loadedNodes_ / (float)asyncProgress_.totalNodes_;
}

void Scene::Update(float timeStep)
{
    if (asyncLoading_)
    {
        UpdateAsyncLoading();
        return;
    }
    
    PROFILE(UpdateScene);
    
    using namespace SceneUpdate;
    
    VariantMap eventData;
    eventData[P_SCENE] = (void*)this;
    eventData[P_TIMESTEP] = timeStep;
    
    // Update variable timestep logic
    SendEvent(E_SCENEUPDATE, eventData);
    
    // Update scene subsystems. If a physics world is present, it will be updated, triggering fixed timestep logic updates
    SendEvent(E_SCENESUBSYSTEMUPDATE, eventData);
    
    // Update smoothing if enabled (network client scenes)
    if (IsSmoothed())
    {
        PROFILE(UpdateSmoothing);
        
        float constant = 1.0f - Clamp(powf(2.0f, -timeStep * smoothingConstant_), 0.0f, 1.0f);
        float squaredSnapThreshold = snapThreshold_ * snapThreshold_;
        
        for (Map<unsigned, Node*>::ConstIterator i = allNodes_.Begin(); i != allNodes_.End() && i->first_ < FIRST_LOCAL_ID; ++i)
            i->second_->UpdateSmoothing(constant, squaredSnapThreshold);
    }
    
    // Post-update variable timestep logic
    SendEvent(E_SCENEPOSTUPDATE, eventData);
}

unsigned Scene::GetFreeNodeID(CreateMode mode)
{
    if (mode == REPLICATED)
    {
        for (;;)
        {
            if (allNodes_.Find(replicatedNodeID_) == allNodes_.End())
                return replicatedNodeID_;
            
            if (replicatedNodeID_ != LAST_REPLICATED_ID)
                ++replicatedNodeID_;
            else
                replicatedNodeID_ = FIRST_REPLICATED_ID;
        }
    }
    else
    {
        for (;;)
        {
            if (allNodes_.Find(localNodeID_) == allNodes_.End())
                return localNodeID_;
            
            if (localNodeID_ != LAST_LOCAL_ID)
                ++localNodeID_;
            else
                localNodeID_ = FIRST_LOCAL_ID;
        }
    }
}

unsigned Scene::GetFreeComponentID(CreateMode mode)
{
    if (mode == REPLICATED)
    {
        for (;;)
        {
            if (allComponents_.Find(replicatedComponentID_) == allComponents_.End())
                return replicatedComponentID_;
            
            if (replicatedComponentID_ != LAST_REPLICATED_ID)
                ++replicatedComponentID_;
            else
                replicatedComponentID_ = FIRST_REPLICATED_ID;
        }
    }
    else
    {
        for (;;)
        {
            if (allComponents_.Find(localComponentID_) == allComponents_.End())
                return localComponentID_;
            
            if (localComponentID_ != LAST_LOCAL_ID)
                ++localComponentID_;
            else
                localComponentID_ = FIRST_LOCAL_ID;
        }
    }
}


void Scene::NodeAdded(Node* node)
{
    if (!node || node->GetScene())
        return;
    
    node->SetScene(this);
    
    // If we already have an existing node with the same ID, must remove the scene reference from it
    unsigned id = node->GetID();
    Map<unsigned, Node*>::Iterator i = allNodes_.Find(id);
    if (i != allNodes_.End() && i->second_ != node)
    {
        LOGWARNING("Overwriting node with ID " + String(id));
        i->second_->SetScene(0);
        i->second_->SetOwner(0);
    }
    
    allNodes_[id] = node;
}

void Scene::NodeRemoved(Node* node)
{
    if (!node || node->GetScene() != this)
        return;
    
    allNodes_.Erase(node->GetID());
    node->SetID(0);
    node->SetScene(0);
}

void Scene::ComponentAdded(Component* component)
{
    if (!component)
        return;
    
    allComponents_[component->GetID()] = component;
}

void Scene::ComponentRemoved(Component* component)
{
    if (!component)
        return;
    
    allComponents_.Erase(component->GetID());
    component->SetID(0);
}

void Scene::HandleUpdate(StringHash eventType, VariantMap& eventData)
{
    using namespace Update;
    
    if (active_)
        Update(eventData[P_TIMESTEP].GetFloat());
}

void Scene::UpdateAsyncLoading()
{
    PROFILE(UpdateAsyncLoading);
    
    Timer asyncLoadTimer;
    
    for (;;)
    {
        if (asyncProgress_.loadedNodes_ >= asyncProgress_.totalNodes_)
        {
            FinishAsyncLoading();
            return;
        }
        
        // Read one child node either from binary or XML
        if (!asyncProgress_.xmlFile_)
        {
            Node* newNode = CreateChild(asyncProgress_.file_->ReadUInt(), REPLICATED);
            newNode->Load(*asyncProgress_.file_);
        }
        else
        {
            Node* newNode = CreateChild(asyncProgress_.xmlElement_.GetInt("id"), REPLICATED);
            newNode->LoadXML(asyncProgress_.xmlElement_);
            asyncProgress_.xmlElement_ = asyncProgress_.xmlElement_.GetNext("node");
        }
        
        ++asyncProgress_.loadedNodes_;
        
        // Break if time limit exceeded, so that we keep sufficient FPS
        if (asyncLoadTimer.GetMSec(false) >= ASYNC_LOAD_MAX_MSEC)
            break;
    }
    
    using namespace AsyncLoadProgress;
    
    VariantMap eventData;
    eventData[P_SCENE] = (void*)this;
    eventData[P_PROGRESS] = (float)asyncProgress_.loadedNodes_ / (float)asyncProgress_.totalNodes_;
    eventData[P_LOADEDNODES]  = asyncProgress_.loadedNodes_;
    eventData[P_TOTALNODES]  = asyncProgress_.totalNodes_;
    SendEvent(E_ASYNCLOADPROGRESS, eventData);
}

void Scene::FinishAsyncLoading()
{
    FinishLoading(asyncProgress_.file_);
    StopAsyncLoading();
    
    using namespace AsyncLoadFinished;
    
    VariantMap eventData;
    eventData[P_SCENE] = (void*)this;
    SendEvent(E_ASYNCLOADFINISHED, eventData);
}

void Scene::FinishLoading(Deserializer* source)
{
    FinishUpdate();
    if (source)
    {
        fileName_ = source->GetName();
        checksum_ = source->GetChecksum();
    }
}

void RegisterSceneLibrary(Context* context)
{
    Node::RegisterObject(context);
    Scene::RegisterObject(context);
}
