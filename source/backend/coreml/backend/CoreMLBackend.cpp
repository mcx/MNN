//
//  CoreMLBackend.cpp
//  MNN
//
//  Created by MNN on 2021/03/24.
//  Copyright © 2018, Alibaba Group Holding Limited
//

#include "CoreMLBackend.hpp"
#include <core/Macro.h>
#include <core/TensorUtils.hpp>
#include <stdlib.h>
#include <MNN/AutoTime.hpp>

extern bool isAvailable();
namespace MNN {
    void registerCoreMLOps();
    static inline std::map<OpType, CoreMLBackend::Creator*>* getCreatorMap() {
        static std::map<OpType, CoreMLBackend::Creator*>* ret = nullptr;
        if (nullptr == ret) {
            ret = new std::map<OpType, CoreMLBackend::Creator*>;
        }
        return ret;
    }

    bool CoreMLBackend::addCreator(OpType t, Creator* c) {
        auto map = getCreatorMap();
        if (map->find(t) != map->end()) {
            MNN_PRINT("Error: %d type has be added\n", t);
            return false;
        }
        map->insert(std::make_pair(t, c));
        return true;
    }

    CoreMLBackend::CoreMLBackend(const CoreMLRuntime* runtime) : Backend(MNN_FORWARD_NN) {
        mNPURuntime = runtime;
        mInputBuffer.root = BufferAllocator::Allocator::createDefault();
        mPrecision  = mNPURuntime->mPrecision;
        mCoreMLExecutor.reset(new CoreMLExecutorWrapper(mPrecision));
        if (mCoreMLModel_ == nullptr) {
            mCoreMLModel_.reset(new _CoreML__Specification__Model);
            core_ml__specification__model__init(mCoreMLModel_.get());
            mCoreMLModel_->specificationversion = 4;
            mCoreMLModel_->type_case = CORE_ML__SPECIFICATION__MODEL__TYPE_NEURAL_NETWORK;
            mCoreMLModel_->neuralnetwork = create<CoreML__Specification__NeuralNetwork>();
            core_ml__specification__neural_network__init(mCoreMLModel_->neuralnetwork);
            mCoreMLModel_->neuralnetwork->arrayinputshapemapping = CORE_ML__SPECIFICATION__NEURAL_NETWORK_MULTI_ARRAY_SHAPE_MAPPING__EXACT_ARRAY_MAPPING;
        }
    }
    CoreMLBackend::~CoreMLBackend() {

    }

    Execution* CoreMLBackend::onCreate(const std::vector<Tensor*>& inputs, const std::vector<Tensor*>& outputs, const MNN::Op* op) {

        auto map = getCreatorMap();
        auto iter = map->find(op->type());
        
        if (iter == map->end()) {
            MNN_PRINT("[CoreML] Don't support type %s.\n", MNN::EnumNameOpType(op->type()));
            return nullptr;
        }

        auto exe = iter->second->onCreate(inputs, outputs, op, this);

        if (nullptr == exe) {
            MNN_PRINT("[CoreML] The Creator Don't support type %s.\n", MNN::EnumNameOpType(op->type()));
            return nullptr;
        }

        return exe;
    }

    void CoreMLBackend::CoreMLBackend::onExecuteBegin() const {
    }
    
    void CoreMLBackend::onExecuteEnd() const {
        invokeModel();
    }

    Backend::MemObj* CoreMLBackend::onAcquire(const Tensor* tensor, StorageType storageType) {
        bool isInputCopy = TensorUtils::getDescribe(tensor)->usage==Tensor::InsideDescribe::Usage::INPUT;
        bool isOutputCopy = TensorUtils::getDescribe(tensor)->usage==Tensor::InsideDescribe::Usage::OUTPUT;
        if(isInputCopy){
            mInputIdxMap.insert(std::make_pair(tensor, mInputIdxMap.size()));
        }
        if(isOutputCopy){
            mOutputIdxMap.insert(std::make_pair(tensor, mOutputIdxMap.size()));
        }
        // Don't need release
        return new Backend::MemObj;
    }

    bool CoreMLBackend::onClearBuffer() {
        return true;
    }
    
    void CoreMLBackend::onCopyBuffer(const Tensor* srcTensor, const Tensor* dstTensor) const {
        if (nullptr == srcTensor->buffer().host || nullptr == dstTensor->buffer().host) {
            MNN_ERROR("[MNN-CoreML]: Invalid copy because not valid input / output\n");
            return;
        }

        bool isInputCopy = TensorUtils::getDescribe(dstTensor)->usage==Tensor::InsideDescribe::Usage::INPUT;
        bool isOutputCopy = TensorUtils::getDescribe(srcTensor)->usage==Tensor::InsideDescribe::Usage::OUTPUT;
        if ((isInputCopy || isOutputCopy) && mPrecision == BackendConfig::Precision_Low) {
            // TODO: Fix bug for int8 with nc4hw4
            ::memcpy(dstTensor->host<void>(), srcTensor->host<void>(),TensorUtils::getRawSize(srcTensor) * sizeof(uint8_t));
            return;
        }
        if (isInputCopy) {
            if (TensorUtils::getDescribe(dstTensor)->dimensionFormat == MNN_DATA_FORMAT_NC4HW4) {
                std::unique_ptr<Tensor> tmp(new Tensor(dstTensor, Tensor::CAFFE, false));
                tmp->buffer().host = dstTensor->buffer().host;
                MNNCPUCopyBuffer(srcTensor, tmp.get());
            } else {
                MNNCPUCopyBuffer(srcTensor, dstTensor);
            }
            return;
        }
        if(isOutputCopy) {
            if (TensorUtils::getDescribe(srcTensor)->dimensionFormat == MNN_DATA_FORMAT_NC4HW4) {
                std::unique_ptr<Tensor> tmp(new Tensor(srcTensor, Tensor::CAFFE, false));
                tmp->buffer().host = srcTensor->buffer().host;
                MNNCPUCopyBuffer(tmp.get(), dstTensor);
            } else {
                MNNCPUCopyBuffer(srcTensor, dstTensor);
            }
        }
    }

    void CoreMLBackend::onResizeBegin() {
        mCoreMLLayerPtrs.clear();
    }
    size_t CoreMLBackend::getBytes(const halide_type_t& type) {
        if (type.code == halide_type_float && mPrecision == BackendConfig::Precision_Low) {
            return 1;
        }
        return static_cast<size_t>(type.bytes());
    }

    ErrorCode CoreMLBackend::onResizeEnd() {
        bool useImage = mPrecision == BackendConfig::Precision_Low;
        size_t allocSize = 0;
        for (auto t : mInputIdxMap) {
            allocSize += (TensorUtils::getRawSize(t.first) * getBytes(t.first->getType()));
        }
        if (useImage) {
            for (auto t : mOutputIdxMap) {
                allocSize += (TensorUtils::getRawSize(t.first) * getBytes(t.first->getType()));
            }
        }
        auto code = mInputBuffer.realloc(allocSize, MNN_MEMORY_ALIGN_DEFAULT);
        if (NO_ERROR != code) {
            return code;
        }
        allocSize = 0;
        auto ptr = mInputBuffer.current.ptr();
        for (auto tt : mInputIdxMap) {
            auto t = (Tensor*)tt.first;
            t->buffer().host = ptr + allocSize;
            allocSize += (TensorUtils::getRawSize(t) * getBytes(t->getType()));
        }
        for (auto tt : mOutputIdxMap) {
            auto t = (Tensor*)tt.first;
            t->buffer().host = ptr + allocSize;
            allocSize += (TensorUtils::getRawSize(t) * getBytes(t->getType()));
        }
        return buildModel();
    }
    bool CoreMLBackend::onUnmapTensor(Tensor::MapType mtype, Tensor::DimensionType dtype, const Tensor* dstTensor, void* mapPtr) {
        return true;
    }

    std::string CoreMLBackend::getTensorName(const Tensor* t) {
        const auto& iter = mTensorIdxMap.find(t);
        if (iter != mTensorIdxMap.end()) {
            //printf("tensorName: %d\n", iter->second);
            return std::to_string(iter->second);
        }
        int idx = static_cast<int>(mTensorIdxMap.size());
        mTensorIdxMap.insert(std::make_pair(t, idx));
        auto idName = std::to_string(idx);
        if (TensorUtils::getDescribe(t)->usage == Tensor::InsideDescribe::CONSTANT) {
            auto constantLayer = create<CoreML__Specification__NeuralNetworkLayer>();
            core_ml__specification__neural_network_layer__init(constantLayer);
            setLayerName(constantLayer, "Constant-" + idName);
            constantLayer->layer_case = CORE_ML__SPECIFICATION__NEURAL_NETWORK_LAYER__LAYER_LOAD_CONSTANT_ND;
            constantLayer->loadconstantnd = create<CoreML__Specification__LoadConstantNDLayerParams>();
            core_ml__specification__load_constant_ndlayer_params__init(constantLayer->loadconstantnd);
            auto shape = t->shape();
            if (shape.size() == 0) {
                shape = {1};
            }
            constantLayer->loadconstantnd->n_shape = shape.size();
            constantLayer->loadconstantnd->shape = create<uint64_t>(constantLayer->loadconstantnd->n_shape);
            for (int i = 0; i < shape.size(); i++) {
                constantLayer->loadconstantnd->shape[i] = shape[i];
            }
            constantLayer->loadconstantnd->data = create<CoreML__Specification__WeightParams>();
            core_ml__specification__weight_params__init(constantLayer->loadconstantnd->data);
            constantLayer->loadconstantnd->data->n_floatvalue = t->elementSize();
            constantLayer->loadconstantnd->data->floatvalue = create<float>(constantLayer->loadconstantnd->data->n_floatvalue);
            memcpy(constantLayer->loadconstantnd->data->floatvalue, t->host<void>(), t->size());
            setLayerOutputs(constantLayer, {idName});
            addLayer(constantLayer);
        }
        return idName;
    }

    void CoreMLBackend::addLayer(CoreML__Specification__NeuralNetworkLayer* layer) {
        mCoreMLLayerPtrs.push_back(layer);
        mCoreMLModel_->neuralnetwork->layers = mCoreMLLayerPtrs.data();
        mCoreMLModel_->neuralnetwork->n_layers = mCoreMLLayerPtrs.size();
    }
    void CoreMLBackend::setLayerName(CoreML__Specification__NeuralNetworkLayer* layer, std::string&& name) {
        copyName(&layer->name, std::move(name));
    }
    void CoreMLBackend::copyName(char** ptr, std::string&& name) {
        *ptr = create<char>(name.size() + 1);
        for (int i = 0; i < name.size(); i++) {
            (*ptr)[i] = name[i];
        }
        (*ptr)[name.size()] = '\0';
    }
    void CoreMLBackend::setLayerInputs(CoreML__Specification__NeuralNetworkLayer* layer, std::vector<std::string>&& inputs) {
        layer->n_input = inputs.size();
        layer->input = create<char*>(layer->n_input);
        for (int i = 0; i < inputs.size(); i++) {
            copyName(&(layer->input[i]), std::move(inputs[i]));
        }
    }
    void CoreMLBackend::setLayerOutputs(CoreML__Specification__NeuralNetworkLayer* layer, std::vector<std::string>&& outputs) {
        layer->n_output = outputs.size();
        layer->output = create<char*>(layer->n_output);
        for (int i = 0; i < outputs.size(); i++) {
            copyName(&(layer->output[i]), std::move(outputs[i]));
        }
    }
    void* CoreMLBackend::onMapTensor(Tensor::MapType mtype, Tensor::DimensionType dtype, const Tensor* srcTensor) {
        return srcTensor->host<void>();
    }

    void CoreMLBackend::setIO(CoreML__Specification__FeatureDescription** describe, const Tensor* t) {
        auto name = getTensorName(t);
        auto des = create<CoreML__Specification__FeatureDescription>();
        core_ml__specification__feature_description__init(des);
        copyName(&(des->name), std::move(name));
        des->type = create<CoreML__Specification__FeatureType>();
        core_ml__specification__feature_type__init(des->type);
        if (mPrecision == BackendConfig::Precision_Low) {
            des->type->type_case = CORE_ML__SPECIFICATION__FEATURE_TYPE__TYPE_IMAGE_TYPE;
            des->type->imagetype = create<CoreML__Specification__ImageFeatureType>();
            core_ml__specification__image_feature_type__init(des->type->imagetype);
            des->type->imagetype->width = t->width();
            des->type->imagetype->height = t->height();
            des->type->imagetype->colorspace = (t->channel() > 1) ? CORE_ML__SPECIFICATION__IMAGE_FEATURE_TYPE__COLOR_SPACE__RGB:
                                               CORE_ML__SPECIFICATION__IMAGE_FEATURE_TYPE__COLOR_SPACE__GRAYSCALE;
            des->type->imagetype->size_flexibility_case = CORE_ML__SPECIFICATION__IMAGE_FEATURE_TYPE__SIZE_FLEXIBILITY__NOT_SET;
        } else {
            des->type->type_case = CORE_ML__SPECIFICATION__FEATURE_TYPE__TYPE_MULTI_ARRAY_TYPE;
            des->type->multiarraytype = create<CoreML__Specification__ArrayFeatureType>();
            core_ml__specification__array_feature_type__init(des->type->multiarraytype);
            des->type->multiarraytype->datatype = CORE_ML__SPECIFICATION__ARRAY_FEATURE_TYPE__ARRAY_DATA_TYPE__FLOAT32;
            auto shape = t->shape();
            des->type->multiarraytype->n_shape = shape.size();
            des->type->multiarraytype->shape = create<int64_t>(des->type->multiarraytype->n_shape);
            for (int i = 0; i < shape.size(); i++) {
                des->type->multiarraytype->shape[i] = shape[i];
            }
        }
        *describe = des;
    }
    ErrorCode CoreMLBackend::buildModel() {
        mCoreMLModel_->description = create<CoreML__Specification__ModelDescription>();
        core_ml__specification__model_description__init(mCoreMLModel_->description);
        mCoreMLModel_->description->n_input = mInputIdxMap.size();
        mCoreMLModel_->description->input = create<CoreML__Specification__FeatureDescription*>(mCoreMLModel_->description->n_input);
        int idx = 0;
        for (const auto& iter : mInputIdxMap) {
            auto t = iter.first;
            setIO(mCoreMLModel_->description->input + idx++, t);
        }
        mCoreMLModel_->description->n_output = mOutputIdxMap.size();
        mCoreMLModel_->description->output = create<CoreML__Specification__FeatureDescription*>(mCoreMLModel_->description->n_output);
        idx = 0;
        for (const auto& iter : mOutputIdxMap) {
            auto t = iter.first;
            setIO(mCoreMLModel_->description->output + idx++, t);
        }
#ifdef DUMP_COREML_GRAPH
        for (int i = 0; i < mCoreMLModel_->neuralnetwork->n_layers; i++) {
            auto layer = mCoreMLModel_->neuralnetwork->layers[i];
            printf("%d : %s@ inputs: [", i, layer->name);
            for (int j = 0; j < layer->n_input; j++)
                printf("%s, ", layer->input[j]);
            printf("], output: [");
            for (int j = 0; j < layer->n_output; j++)
                printf("%s, ", layer->output[j]);
            printf("]\n");
        }
#endif
        if (mCoreMLModel_->neuralnetwork->n_layers <= 0) {
            return NO_EXECUTION;
        }
        bool success = mCoreMLExecutor->compileModel(mCoreMLModel_.get());
        if (success) {
            return NO_ERROR;
        } else {
            return NO_EXECUTION;
        }
    }
    void CoreMLBackend::invokeModel() const {
        if (mCoreMLModel_->neuralnetwork->n_layers <= 0) {
            return;
        }
        std::vector<std::pair<const MNN::Tensor*, std::string>> inputs(mInputIdxMap.size()), outputs(mOutputIdxMap.size());
        // get names
        for (const auto& iter : mInputIdxMap) {
            auto t = iter.first;
            auto idx = iter.second;
            inputs[idx].first = t;
            inputs[idx].second = std::to_string(mTensorIdxMap.find(t)->second);
        }
        for (const auto& iter : mOutputIdxMap) {
            auto t = iter.first;
            auto idx = iter.second;
            outputs[idx].first = t;
            outputs[idx].second = std::to_string(mTensorIdxMap.find(t)->second);
        }
        mCoreMLExecutor->invokModel(inputs, outputs);
    }
    CoreMLRuntime::CoreMLRuntime(const Backend::Info& info) {
        mInfo = info;
        BackendConfig::PrecisionMode precision = BackendConfig::Precision_Normal;
        BackendConfig::PowerMode power         = BackendConfig::Power_Normal;
        if (nullptr != mInfo.user) {
            precision = mInfo.user->precision;
            power     = mInfo.user->power;
        }

        mPrecision = precision;
    }

    CoreMLRuntime::~CoreMLRuntime() {}

    Backend* CoreMLRuntime::onCreate(const BackendConfig* config, Backend* origin) const {
        return new CoreMLBackend(this);
    }

    void CoreMLRuntime::onGabageCollect(int level) {
        // nothing now
    }
    CoreMLRuntime::CompilerType CoreMLRuntime::onGetCompilerType() const {
        return Compiler_Geometry;
    }

    struct CoreMLBackendCreator : RuntimeCreator {

        virtual Runtime* onCreate(const Backend::Info& info) const override {
            return new CoreMLRuntime(info);
        }

        virtual bool onValid(Backend::Info& info) const override {
            return true;
        }
    };

    void registerCoreMLRuntimeCreator() {
        if (!isAvailable()) {
            return;
        }
        registerCoreMLOps();
        MNNInsertExtraRuntimeCreator(MNN_FORWARD_NN, new CoreMLBackendCreator, false);
    }
}
