/**
MIT License

Copyright (c) 2018 NVIDIA CORPORATION. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*
*/

#include "yolo.h"
#include <memory>
#include <vector>
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
Yolo::Yolo(const uint32_t batchSize, const NetworkInfo& networkInfo, const InferParams& inferParams) :
    m_EnginePath(networkInfo.enginePath),
    m_NetworkType(networkInfo.networkType),
    m_ConfigFilePath(networkInfo.configFilePath),
    m_WtsFilePath(networkInfo.wtsFilePath),
    m_LabelsFilePath(networkInfo.labelsFilePath),
    m_Precision(networkInfo.precision),
    m_DeviceType(networkInfo.deviceType),
    m_CalibImages(inferParams.calibImages),
    m_CalibImagesFilePath(inferParams.calibImagesPath),
    m_CalibTableFilePath(networkInfo.calibrationTablePath),
    m_InputBlobName(networkInfo.inputBlobName),
    m_InputH(0),
    m_InputW(0),
    m_InputC(0),
    m_InputSize(0),
    m_ProbThresh(inferParams.probThresh),
    m_NMSThresh(inferParams.nmsThresh),
    m_PrintPerfInfo(inferParams.printPerfInfo),
    m_PrintPredictions(inferParams.printPredictionInfo),
    m_Logger(Logger()),
    m_BatchSize(batchSize),
    m_Network(nullptr),
    m_Builder(nullptr),
    m_ModelStream(nullptr),
    m_Engine(nullptr),
    m_Context(nullptr),
    m_InputBindingIndex(-1),
    m_CudaStream(nullptr),
    m_PluginFactory(new PluginFactory),
    m_TinyMaxpoolPaddingFormula(new YoloTinyMaxpoolPaddingFormula)
{
   // m_ClassNames = loadListFromTextFile(m_LabelsFilePath);
	m_configBlocks = parseConfigFile(m_ConfigFilePath);
	if (m_NetworkType == "yolov5")
	{
		parse_cfg_blocks_v5(m_configBlocks);
	}
	else
	{
		parseConfigBlocks();
	}
	
    if (m_Precision == "kFLOAT")
    {
		if ("yolov5" == m_NetworkType)
		{

			create_engine_yolov5();
		}
		else
		{
			createYOLOEngine();
		}
    }
    else if (m_Precision == "kINT8")
    {
        Int8EntropyCalibrator calibrator(m_BatchSize, m_CalibImages, m_CalibImagesFilePath,
                                         m_CalibTableFilePath, m_InputSize, m_InputH, m_InputW,
                                         m_InputBlobName);
		if ("yolov5" == m_NetworkType)
		{
			create_engine_yolov5(nvinfer1::DataType::kINT8, &calibrator);
		}
		else
		{
			createYOLOEngine(nvinfer1::DataType::kINT8, &calibrator);
		}
    }
    else if (m_Precision == "kHALF")
    {
		if ("yolov5" == m_NetworkType)
		{
			create_engine_yolov5(nvinfer1::DataType::kHALF, nullptr);
		}
		else
		{
			createYOLOEngine(nvinfer1::DataType::kHALF, nullptr);
		}
    }
    else
    {
        std::cout << "Unrecognized precision type " << m_Precision << std::endl;
        assert(0);
    }
	if (m_NetworkType == "yolov4" || m_NetworkType  == "yolov4-tiny")
	{
	}
    assert(m_PluginFactory != nullptr);
    m_Engine = loadTRTEngine(m_EnginePath, m_PluginFactory, m_Logger);
    assert(m_Engine != nullptr);
    m_Context = m_Engine->createExecutionContext();
    assert(m_Context != nullptr);
    m_InputBindingIndex = m_Engine->getBindingIndex(m_InputBlobName.c_str());
    assert(m_InputBindingIndex != -1);
    assert(m_BatchSize <= static_cast<uint32_t>(m_Engine->getMaxBatchSize()));
    allocateBuffers();
    NV_CUDA_CHECK(cudaStreamCreate(&m_CudaStream));
    assert(verifyYoloEngine());
};

Yolo::~Yolo()
{
    for (auto& tensor : m_OutputTensors) NV_CUDA_CHECK(cudaFreeHost(tensor.hostBuffer));
    for (auto& deviceBuffer : m_DeviceBuffers) NV_CUDA_CHECK(cudaFree(deviceBuffer));
    NV_CUDA_CHECK(cudaStreamDestroy(m_CudaStream));
    if (m_Context)
    {
        m_Context->destroy();
        m_Context = nullptr;
    }

    if (m_Engine)
    {
        m_Engine->destroy();
        m_Engine = nullptr;
    }

    if (m_PluginFactory)
    {
        m_PluginFactory->destroy();
        m_PluginFactory = nullptr;
    }

    m_TinyMaxpoolPaddingFormula.reset();
}

std::vector<int> split_layer_index(const std::string &s_,const std::string &delimiter_)
{
	std::vector<int> index;
	std::string s = s_;
	size_t pos = 0;
	std::string token;
	while ((pos = s.find(delimiter_)) != std::string::npos) 
	{
		token = s.substr(0, pos);
		index.push_back(std::stoi(trim(token)));
		s.erase(0, pos + delimiter_.length());
	}
	index.push_back(std::stoi(trim(s)));
	return index;
}

void Yolo::createYOLOEngine(const nvinfer1::DataType dataType, Int8EntropyCalibrator* calibrator)
{
	std::vector<float> weights = loadWeights(m_WtsFilePath, m_NetworkType);
    std::vector<nvinfer1::Weights> trtWeights;
    int weightPtr = 0;
    int channels = m_InputC;
	m_Builder = nvinfer1::createInferBuilder(m_Logger);
	nvinfer1::IBuilderConfig* config = m_Builder->createBuilderConfig();
    m_Network = m_Builder->createNetworkV2(0U);
    if ((dataType == nvinfer1::DataType::kINT8 && !m_Builder->platformHasFastInt8())
        || (dataType == nvinfer1::DataType::kHALF && !m_Builder->platformHasFastFp16()))
    {
        std::cout << "Platform doesn't support this precision." << std::endl;
        assert(0);
    }

    nvinfer1::ITensor* data = m_Network->addInput(
        m_InputBlobName.c_str(), nvinfer1::DataType::kFLOAT,
        nvinfer1::DimsCHW{static_cast<int>(m_InputC), static_cast<int>(m_InputH),
                          static_cast<int>(m_InputW)});
    assert(data != nullptr);
    // Add elementwise layer to normalize pixel values 0-1
    nvinfer1::Dims divDims{
        3,
        {static_cast<int>(m_InputC), static_cast<int>(m_InputH), static_cast<int>(m_InputW)},
        {nvinfer1::DimensionType::kCHANNEL, nvinfer1::DimensionType::kSPATIAL,
         nvinfer1::DimensionType::kSPATIAL}};
    nvinfer1::Weights divWeights{nvinfer1::DataType::kFLOAT, nullptr,
                                 static_cast<int64_t>(m_InputSize)};
    float* divWt = new float[m_InputSize];
    for (uint32_t w = 0; w < m_InputSize; ++w) divWt[w] = 255.0;
    divWeights.values = divWt;
    trtWeights.push_back(divWeights);
    nvinfer1::IConstantLayer* constDivide = m_Network->addConstant(divDims, divWeights);
    assert(constDivide != nullptr);
    nvinfer1::IElementWiseLayer* elementDivide = m_Network->addElementWise(
        *data, *constDivide->getOutput(0), nvinfer1::ElementWiseOperation::kDIV);
    assert(elementDivide != nullptr);

    nvinfer1::ITensor* previous = elementDivide->getOutput(0);
    std::vector<nvinfer1::ITensor*> tensorOutputs;
    uint32_t outputTensorCount = 0;

	if ("yolov3" == m_NetworkType || "yolov3-tiny" == m_NetworkType)
	{
		// Set the output dimensions formula for pooling layers
		assert(m_TinyMaxpoolPaddingFormula && "Tiny maxpool padding formula not created");
		m_Network->setPoolingOutputDimensionsFormula(m_TinyMaxpoolPaddingFormula.get());
	}

    // build the network using the network API
    for (uint32_t i = 0; i < m_configBlocks.size(); ++i)
    {
		// check if num. of channels is correct
        assert(getNumChannels(previous) == channels);
        std::string layerIndex = "(" + std::to_string(i) + ")";

        if (m_configBlocks.at(i).at("type") == "net")
        {
            printLayerInfo("", "layer", "     inp_size", "     out_size", "weightPtr");
        }
        else if (m_configBlocks.at(i).at("type") == "convolutional")
        {
            std::string inputVol = dimsToString(previous->getDimensions());
            nvinfer1::ILayer* out;
            std::string layerType;
			//check activation
			std::string activation = "";
			if (m_configBlocks.at(i).find("activation") != m_configBlocks.at(i).end())
			{
				activation = m_configBlocks[i]["activation"];
			}
            // check if batch_norm enabled
            if ((m_configBlocks.at(i).find("batch_normalize") != m_configBlocks.at(i).end()) &&
				("leaky" == activation))
            {
                out = netAddConvBNLeaky(i, m_configBlocks.at(i), weights, trtWeights, weightPtr,
                                        channels, previous, m_Network);
                layerType = "conv-bn-leaky";
            }
			else if ((m_configBlocks.at(i).find("batch_normalize") != m_configBlocks.at(i).end()) &&
				("mish" == activation))
			{
				out = net_conv_bn_mish(i, m_configBlocks.at(i), weights, trtWeights, weightPtr,
										channels, previous, m_Network);
				layerType = "conv-bn-mish";
			}
            else// if("linear" == activation)
            {
                out = netAddConvLinear(i, m_configBlocks.at(i), weights, trtWeights, weightPtr,
                                       channels, previous, m_Network);
                layerType = "conv-linear";
            }
            previous = out->getOutput(0);
            assert(previous != nullptr);
            channels = getNumChannels(previous);
            std::string outputVol = dimsToString(previous->getDimensions());
            tensorOutputs.push_back(out->getOutput(0));
            printLayerInfo(layerIndex, layerType, inputVol, outputVol, std::to_string(weightPtr));
        }
        else if (m_configBlocks.at(i).at("type") == "shortcut")
        {
            assert(m_configBlocks.at(i).at("activation") == "linear");
            assert(m_configBlocks.at(i).find("from") != m_configBlocks.at(i).end());
            int from = stoi(m_configBlocks.at(i).at("from"));

            std::string inputVol = dimsToString(previous->getDimensions());
            // check if indexes are correct
            assert((i - 2 >= 0) && (i - 2 < tensorOutputs.size()));
            assert((i + from - 1 >= 0) && (i + from - 1 < tensorOutputs.size()));
            assert(i + from - 1 < i - 2);
            nvinfer1::IElementWiseLayer* ew
                = m_Network->addElementWise(*tensorOutputs[i - 2], *tensorOutputs[i + from - 1],
                                            nvinfer1::ElementWiseOperation::kSUM);
            assert(ew != nullptr);
            std::string ewLayerName = "shortcut_" + std::to_string(i);
            ew->setName(ewLayerName.c_str());
            previous = ew->getOutput(0);
            assert(previous != nullptr);
            std::string outputVol = dimsToString(previous->getDimensions());
            tensorOutputs.push_back(ew->getOutput(0));
            printLayerInfo(layerIndex, "skip", inputVol, outputVol, "    -");
        }
        else if (m_configBlocks.at(i).at("type") == "yolo")
        {
            nvinfer1::Dims prevTensorDims = previous->getDimensions();
            assert(prevTensorDims.d[1] == prevTensorDims.d[2]);
            TensorInfo& curYoloTensor = m_OutputTensors.at(outputTensorCount);
            curYoloTensor.gridSize = prevTensorDims.d[1];
            curYoloTensor.stride = m_InputW / curYoloTensor.gridSize;
            m_OutputTensors.at(outputTensorCount).volume = curYoloTensor.gridSize
                * curYoloTensor.gridSize
                * (curYoloTensor.numBBoxes * (5 + curYoloTensor.numClasses));
            std::string layerName = "yolo_" + std::to_string(i);
            curYoloTensor.blobName = layerName;
            nvinfer1::IPlugin* yoloPlugin
                = new YoloLayerV3(m_OutputTensors.at(outputTensorCount).numBBoxes,
                                  m_OutputTensors.at(outputTensorCount).numClasses,
                                  m_OutputTensors.at(outputTensorCount).gridSize);
            assert(yoloPlugin != nullptr);
            nvinfer1::IPluginLayer* yolo = m_Network->addPlugin(&previous, 1, *yoloPlugin);
            assert(yolo != nullptr);
            yolo->setName(layerName.c_str());
            std::string inputVol = dimsToString(previous->getDimensions());
            previous = yolo->getOutput(0);
            assert(previous != nullptr);
            previous->setName(layerName.c_str());
            std::string outputVol = dimsToString(previous->getDimensions());
            m_Network->markOutput(*previous);
            channels = getNumChannels(previous);
            tensorOutputs.push_back(yolo->getOutput(0));
            printLayerInfo(layerIndex, "yolo", inputVol, outputVol, std::to_string(weightPtr));
            ++outputTensorCount;
        }
        else if (m_configBlocks.at(i).at("type") == "route")
        {
            size_t found = m_configBlocks.at(i).at("layers").find(",");
            if (found != std::string::npos)//concate multi layers 
            {
				std::vector<int> vec_index = split_layer_index(m_configBlocks.at(i).at("layers"), ",");
				for (auto &ind_layer:vec_index)
				{
					if (ind_layer < 0)
					{
						ind_layer = tensorOutputs.size() + ind_layer;
					}
					assert(ind_layer < static_cast<int>(tensorOutputs.size()) && ind_layer >= 0);
				}
                nvinfer1::ITensor** concatInputs
                    = reinterpret_cast<nvinfer1::ITensor**>(malloc(sizeof(nvinfer1::ITensor*) * vec_index.size()));
				for (int ind=0;ind<vec_index.size();++ind)
				{
					concatInputs[ind] = tensorOutputs[vec_index[ind]];
				}
                nvinfer1::IConcatenationLayer* concat
                    = m_Network->addConcatenation(concatInputs, vec_index.size());
                assert(concat != nullptr);
                std::string concatLayerName = "route_" + std::to_string(i - 1);
                concat->setName(concatLayerName.c_str());
                // concatenate along the channel dimension
                concat->setAxis(0);
                previous = concat->getOutput(0);
                assert(previous != nullptr);
				nvinfer1::Dims debug = previous->getDimensions();
                std::string outputVol = dimsToString(previous->getDimensions());
				int nums = 0;
				for (auto &indx:vec_index)
				{
					nums += getNumChannels(tensorOutputs[indx]);
				}
				channels = nums;
                tensorOutputs.push_back(concat->getOutput(0));
                printLayerInfo(layerIndex, "route", "        -", outputVol,std::to_string(weightPtr));
            }
            else //single layer
            {
                int idx = std::stoi(trim(m_configBlocks.at(i).at("layers")));
                if (idx < 0)
                {
                    idx = tensorOutputs.size() + idx;
                }
                assert(idx < static_cast<int>(tensorOutputs.size()) && idx >= 0);

				//route
				if (m_configBlocks.at(i).find("groups") == m_configBlocks.at(i).end())
				{
					previous = tensorOutputs[idx];
					assert(previous != nullptr);
					std::string outputVol = dimsToString(previous->getDimensions());
					// set the output volume depth
					channels = getNumChannels(tensorOutputs[idx]);
					tensorOutputs.push_back(tensorOutputs[idx]);
					printLayerInfo(layerIndex, "route", "        -", outputVol, std::to_string(weightPtr));

				}
				//yolov4-tiny route split layer
				else
				{
					if (m_configBlocks.at(i).find("group_id") == m_configBlocks.at(i).end())
					{
						assert(0);
					}
					int chunk_idx = std::stoi(trim(m_configBlocks.at(i).at("group_id")));
					nvinfer1::ILayer* out = layer_split(i, tensorOutputs[idx], m_Network);
					std::string inputVol = dimsToString(previous->getDimensions());
					previous = out->getOutput(chunk_idx);
					assert(previous != nullptr);
					channels = getNumChannels(previous);
					std::string outputVol = dimsToString(previous->getDimensions());
					tensorOutputs.push_back(out->getOutput(chunk_idx));
					printLayerInfo(layerIndex,"chunk", inputVol, outputVol, std::to_string(weightPtr));
				}
            }
        }
        else if (m_configBlocks.at(i).at("type") == "upsample")
        {
            std::string inputVol = dimsToString(previous->getDimensions());
            nvinfer1::ILayer* out = netAddUpsample(i - 1, m_configBlocks[i], weights, trtWeights,
                                                   channels, previous, m_Network);
            previous = out->getOutput(0);
            std::string outputVol = dimsToString(previous->getDimensions());
            tensorOutputs.push_back(out->getOutput(0));
            printLayerInfo(layerIndex, "upsample", inputVol, outputVol, "    -");
        }
        else if (m_configBlocks.at(i).at("type") == "maxpool")
        {
            // Add same padding layers
            if (m_configBlocks.at(i).at("size") == "2" && m_configBlocks.at(i).at("stride") == "1")
            {
                m_TinyMaxpoolPaddingFormula->addSamePaddingLayer("maxpool_" + std::to_string(i));
            }
            std::string inputVol = dimsToString(previous->getDimensions());
            nvinfer1::ILayer* out = netAddMaxpool(i, m_configBlocks.at(i), previous, m_Network);
            previous = out->getOutput(0);
            assert(previous != nullptr);
            std::string outputVol = dimsToString(previous->getDimensions());
            tensorOutputs.push_back(out->getOutput(0));
            printLayerInfo(layerIndex, "maxpool", inputVol, outputVol, std::to_string(weightPtr));
        }
        else
        {
            std::cout << "Unsupported layer type --> \"" << m_configBlocks.at(i).at("type") << "\""
                      << std::endl;
            assert(0);
        }
    }

    if (weights.size() != weightPtr)
    {
        std::cout << "Number of unused weights left : " << weights.size() - weightPtr << std::endl;
        assert(0);
    }

 //   std::cout << "Output blob names :" << std::endl;
 //   for (auto& tensor : m_OutputTensors) std::cout << tensor.blobName << std::endl;

    // Create and cache the engine if not already present
    if (fileExists(m_EnginePath))
    {
        std::cout << "Using previously generated plan file located at " << m_EnginePath
                  << std::endl;
        destroyNetworkUtils(trtWeights);
        return;
    }

	/*std::cout << "Unable to find cached TensorRT engine for network : " << m_NetworkType
			  << " precision : " << m_Precision << " and batch size :" << m_BatchSize << std::endl;*/

    m_Builder->setMaxBatchSize(m_BatchSize);
    m_Builder->setMaxWorkspaceSize(1 << 20);

    if (dataType == nvinfer1::DataType::kINT8)
    {
        assert((calibrator != nullptr) && "Invalid calibrator for INT8 precision");
      //  m_Builder->setInt8Mode(true);
		config->setFlag(nvinfer1::BuilderFlag::kINT8);
     //   m_Builder->setInt8Calibrator(calibrator);
		config->setInt8Calibrator(calibrator);
    }
    else if (dataType == nvinfer1::DataType::kHALF)
    {
		config->setFlag(nvinfer1::BuilderFlag::kFP16);
     //   m_Builder->setHalf2Mode(true);
    }

    m_Builder->allowGPUFallback(true);
    int nbLayers = m_Network->getNbLayers();
    int layersOnDLA = 0;
 //   std::cout << "Total number of layers: " << nbLayers << std::endl;
    for (uint32_t i = 0; i < nbLayers; i++)
    {
        nvinfer1::ILayer* curLayer = m_Network->getLayer(i);
        if (m_DeviceType == "kDLA" && m_Builder->canRunOnDLA(curLayer))
        {
            m_Builder->setDeviceType(curLayer, nvinfer1::DeviceType::kDLA);
            layersOnDLA++;
            std::cout << "Set layer " << curLayer->getName() << " to run on DLA" << std::endl;
        }
    }
 //   std::cout << "Total number of layers on DLA: " << layersOnDLA << std::endl;

    // Build the engine
    std::cout << "Building the TensorRT Engine..." << std::endl;
    m_Engine = m_Builder->buildEngineWithConfig(*m_Network,*config);
    assert(m_Engine != nullptr);
    std::cout << "Building complete!" << std::endl;

    // Serialize the engine
    writePlanFileToDisk();

    // destroy
    destroyNetworkUtils(trtWeights);
}

int make_division(const float f_in_, const int n_divisor_)
{
	return ceil(f_in_ / n_divisor_)*n_divisor_;
}

void parse_bottleneck_args(const std::string s_args_, int &n_out_ch_, bool &b_shourt_cut_)
{
	std::string s_args = s_args_;
	while (!s_args.empty())
	{
		int npos = s_args.find_first_of(',');
		if (npos != -1)
		{
			n_out_ch_ = std::stoi(trim(s_args.substr(0, npos)));
			s_args.erase(0, npos + 1);
		}
		else
		{
			try
			{
				n_out_ch_ = std::stoi(trim(s_args.substr(0, npos)));
			}
			catch (const std::exception&)
			{

			}
			if ("False" == trim(s_args))
			{
				b_shourt_cut_ = false;
			}
			else if ("True" == trim(s_args))
			{
				b_shourt_cut_ = true;
			}
			break;
		}
	}
}

float round_f(const float in_, const int precision_)
{
	float out;
	std::stringstream ss;
	ss << std::setprecision(precision_) << in_;
	ss >> out;
	return out;
}

void Yolo::create_engine_yolov5(const nvinfer1::DataType dataType,
	Int8EntropyCalibrator* calibrator )
{
	std::map<std::string, std::vector<float>> model_wts;
	load_weights_v5(m_WtsFilePath, model_wts);
	std::vector<nvinfer1::Weights> trtWeights;
	int channels = m_InputC;
	m_Builder = nvinfer1::createInferBuilder(m_Logger);
	nvinfer1::IBuilderConfig* config = m_Builder->createBuilderConfig();
	m_Network = m_Builder->createNetworkV2(0U);
	if ((dataType == nvinfer1::DataType::kINT8 && !m_Builder->platformHasFastInt8())
		|| (dataType == nvinfer1::DataType::kHALF && !m_Builder->platformHasFastFp16()))
	{
		std::cout << "Platform doesn't support this precision." << std::endl;
		assert(0);
	}

	nvinfer1::ITensor* data = m_Network->addInput(
		m_InputBlobName.c_str(),
		nvinfer1::DataType::kFLOAT,
		nvinfer1::DimsCHW{ static_cast<int>(m_InputC), static_cast<int>(m_InputH),
		static_cast<int>(m_InputW) });
	assert(data != nullptr);
	// Add elementwise layer to normalize pixel values 0-1
	nvinfer1::Dims divDims{
		3,
		{ static_cast<int>(m_InputC), static_cast<int>(m_InputH), static_cast<int>(m_InputW) },
		{ nvinfer1::DimensionType::kCHANNEL, nvinfer1::DimensionType::kSPATIAL,
		nvinfer1::DimensionType::kSPATIAL } };

	nvinfer1::Weights divWeights{ nvinfer1::DataType::kFLOAT,
		nullptr,
		static_cast<int64_t>(m_InputSize) };
	float* divWt = new float[m_InputSize];
	for (uint32_t w = 0; w < m_InputSize; ++w) divWt[w] = 255.0;
	divWeights.values = divWt;
	trtWeights.push_back(divWeights);
	nvinfer1::IConstantLayer* constDivide = m_Network->addConstant(divDims, divWeights);
	assert(constDivide != nullptr);
	nvinfer1::IElementWiseLayer* elementDivide = m_Network->addElementWise(
		*data, *constDivide->getOutput(0), nvinfer1::ElementWiseOperation::kDIV);
	assert(elementDivide != nullptr);

	nvinfer1::ITensor* previous = elementDivide->getOutput(0);
	std::vector<nvinfer1::ITensor*> tensorOutputs;
	int n_layer_wts_index = 0;
	int n_output = 3 * (_n_classes + 5);
	for (uint32_t i = 0; i < m_configBlocks.size(); ++i)
	{
		assert(getNumChannels(previous) == channels);
		std::string layerIndex = "(" + std::to_string(i) + ")";

		if ("net" == m_configBlocks.at(i).at("type") )
		{
			printLayerInfo("", "layer", "     inp_size", "     out_size","");
		}
		else if ("Focus" == m_configBlocks.at(i).at("type"))
		{
			std::string inputVol = dimsToString(previous->getDimensions());
			int ptr = 0;
			std::vector<int> args = parse_int_list(m_configBlocks[i]["args"]);
			int filters = args[0];
			int kernel_size = args[1];
			int n_out_channel = (n_output != filters) ? make_division(filters*_f_width_multiple, 8) : filters;
			nvinfer1::ILayer* out = layer_focus(i,
				m_configBlocks.at(i),
				model_wts,
				previous,
				n_out_channel,
				kernel_size,
				trtWeights,
				m_Network);
			previous = out->getOutput(0);
			assert(previous != nullptr);
			channels = getNumChannels(previous);
			std::string outputVol = dimsToString(previous->getDimensions());
			tensorOutputs.push_back(out->getOutput(0));
			printLayerInfo(layerIndex,"Focus", inputVol, outputVol, "");
		}//end focus
		else if ("Conv" == m_configBlocks.at(i).at("type"))
		{
			std::string inputVol = dimsToString(previous->getDimensions());
			int ptr = 0;
			std::vector<int> args = parse_int_list(m_configBlocks[i]["args"]);
			int filters = args[0];
			int kernel_size = args[1];
			int sride = args[2];
			int n_out_channel = (n_output != filters) ? make_division(filters*_f_width_multiple, 8) : filters;
			nvinfer1::ILayer * out = layer_conv_bn_act("model."+std::to_string(i-1), model_wts, previous, m_Network, n_out_channel, kernel_size, sride);
			previous = out->getOutput(0);
			assert(previous != nullptr);
			channels = getNumChannels(previous);
			std::string outputVol = dimsToString(previous->getDimensions());
			tensorOutputs.push_back(out->getOutput(0));
			printLayerInfo(layerIndex, "Conv", inputVol, outputVol, "");
		}//end Conv
		else if ("BottleneckCSP" == m_configBlocks.at(i).at("type"))
		{
			std::string inputVol = dimsToString(previous->getDimensions());
			int ptr = 0;
			int filters = 0;
			bool short_cut = false;
			int number = std::stoi(m_configBlocks[i]["number"]);
			parse_bottleneck_args(m_configBlocks[i]["args"], filters, short_cut);
			int n_out_channel = (n_output != filters) ? make_division(filters*_f_width_multiple, 8) : filters;
			int n_depth = (number > 1) ? (std::max(int(round(_f_depth_multiple *number)), 1)) : number;
			std::string s_model_name = "model." + std::to_string(i- 1);
			auto out = layer_bottleneck_csp(s_model_name, model_wts, m_Network, previous, n_out_channel, n_depth, short_cut);
			previous = out->getOutput(0);
			assert(previous != nullptr);
			channels = getNumChannels(previous);
			std::string outputVol = dimsToString(previous->getDimensions());
			tensorOutputs.push_back(out->getOutput(0));
			printLayerInfo(layerIndex, "BottleneckCSP", inputVol, outputVol, "");
		}// bottleneckCSP
		else if ("SPP" == m_configBlocks.at(i).at("type"))
		{
		}//end SPP
		else if ("nn.Upsample" == m_configBlocks.at(i).at("type"))
		{
		}//end upsample
		else if ("Concat" == m_configBlocks.at(i).at("type"))
		{
		}//end concat
		else if ("Detect" == m_configBlocks.at(i).at("type"))
		{
		}//end detect
	}

}

void Yolo::load_weights_v5(const std::string s_weights_path_,std::map<std::string,std::vector<float>> &vec_wts_)
{
	vec_wts_.clear();
	assert(fileExists(s_weights_path_));
	std::cout << "Loading pre-trained weights..." << std::endl;
	std::ifstream file(s_weights_path_, std::ios_base::binary);
	assert(file.good());
	std::string line;
	while (std::getline(file,line))
	{
		if(line.size()==0)continue;
		std::stringstream iss(line);
		std::string wts_name;
		iss >> wts_name ;
		std::vector<float> weights;
		uint32_t n_str;
		while(iss >> std::hex >> n_str)
		{
			weights.push_back(reinterpret_cast<float&>(n_str));
		}
		vec_wts_[wts_name] = weights;
	}
	std::cout << "Loading complete!" << std::endl;
}
void Yolo::doInference(const unsigned char* input, const uint32_t batchSize)
{
//	Timer timer;
    assert(batchSize <= m_BatchSize && "Image batch size exceeds TRT engines batch size");
    NV_CUDA_CHECK(cudaMemcpyAsync(m_DeviceBuffers.at(m_InputBindingIndex), input,
                                  batchSize * m_InputSize * sizeof(float), cudaMemcpyHostToDevice,
                                  m_CudaStream));
	
    m_Context->enqueue(batchSize, m_DeviceBuffers.data(), m_CudaStream, nullptr);
    for (auto& tensor : m_OutputTensors)
    {
        NV_CUDA_CHECK(cudaMemcpyAsync(tensor.hostBuffer, m_DeviceBuffers.at(tensor.bindingIndex),
                                      batchSize * tensor.volume * sizeof(float),
                                      cudaMemcpyDeviceToHost, m_CudaStream));
    }
    cudaStreamSynchronize(m_CudaStream);
//	timer.out("inference");
}

std::vector<BBoxInfo> Yolo::decodeDetections(const int& imageIdx,
										     const int& imageH,
                                             const int& imageW)
{
    std::vector<BBoxInfo> binfo;
    for (auto& tensor : m_OutputTensors)
    {
        std::vector<BBoxInfo> curBInfo = decodeTensor(imageIdx, imageH, imageW, tensor);
        binfo.insert(binfo.end(), curBInfo.begin(), curBInfo.end());
    }
    return binfo;
}

std::vector<std::map<std::string, std::string>> Yolo::parseConfigFile(const std::string cfgFilePath)
{
    assert(fileExists(cfgFilePath));
    std::ifstream file(cfgFilePath);
    assert(file.good());
    std::string line;
    std::vector<std::map<std::string, std::string>> blocks;
    std::map<std::string, std::string> block;

    while (getline(file, line))
    {
        if (line.size() == 0) continue;
        if (line.front() == '#') continue;
        line = trim(line);
        if (line.front() == '[')
        {
            if (block.size() > 0)
            {
                blocks.push_back(block);
                block.clear();
            }
            std::string key = "type";
            std::string value = trim(line.substr(1, line.size() - 2));
            block.insert(std::pair<std::string, std::string>(key, value));
        }
        else
        {
            int cpos = line.find('=');
            std::string key = trim(line.substr(0, cpos));
            std::string value = trim(line.substr(cpos + 1));
            block.insert(std::pair<std::string, std::string>(key, value));
        }
    }
    blocks.push_back(block);
    return blocks;
}

void Yolo::parseConfigBlocks()
{
    for (auto block : m_configBlocks)
    {
        if (block.at("type") == "net")
        {
            assert((block.find("height") != block.end())
                   && "Missing 'height' param in network cfg");
            assert((block.find("width") != block.end()) && "Missing 'width' param in network cfg");
            assert((block.find("channels") != block.end())
                   && "Missing 'channels' param in network cfg");

            m_InputH = std::stoul(block.at("height"));
            m_InputW = std::stoul(block.at("width"));
            m_InputC = std::stoul(block.at("channels"));
            assert(m_InputW == m_InputH);
            m_InputSize = m_InputC * m_InputH * m_InputW;
        }
        else if ((block.at("type") == "region") || (block.at("type") == "yolo"))
        {
            assert((block.find("num") != block.end())
                   && std::string("Missing 'num' param in " + block.at("type") + " layer").c_str());
            assert((block.find("classes") != block.end())
                   && std::string("Missing 'classes' param in " + block.at("type") + " layer")
                          .c_str());
            assert((block.find("anchors") != block.end())
                   && std::string("Missing 'anchors' param in " + block.at("type") + " layer")
                          .c_str());

            TensorInfo outputTensor;
            std::string anchorString = block.at("anchors");
            while (!anchorString.empty())
            {
                int npos = anchorString.find_first_of(',');
                if (npos != -1)
                {
                    float anchor = std::stof(trim(anchorString.substr(0, npos)));
                    outputTensor.anchors.push_back(anchor);
                    anchorString.erase(0, npos + 1);
                }
                else
                {
                    float anchor = std::stof(trim(anchorString));
                    outputTensor.anchors.push_back(anchor);
                    break;
                }
            }

            if ((m_NetworkType == "yolov3") ||
				(m_NetworkType == "yolov3-tiny") ||
				(m_NetworkType == "yolov4") ||
				(m_NetworkType == "yolov4-tiny"))
            {
                assert((block.find("mask") != block.end())
                       && std::string("Missing 'mask' param in " + block.at("type") + " layer")
                              .c_str());

                std::string maskString = block.at("mask");
                while (!maskString.empty())
                {
                    int npos = maskString.find_first_of(',');
                    if (npos != -1)
                    {
                        uint32_t mask = std::stoul(trim(maskString.substr(0, npos)));
                        outputTensor.masks.push_back(mask);
                        maskString.erase(0, npos + 1);
                    }
                    else
                    {
                        uint32_t mask = std::stoul(trim(maskString));
                        outputTensor.masks.push_back(mask);
                        break;
                    }
                }
            }

            outputTensor.numBBoxes = outputTensor.masks.size() > 0
                ? outputTensor.masks.size()
                : std::stoul(trim(block.at("num")));
            outputTensor.numClasses = std::stoul(block.at("classes"));
            if (m_ClassNames.empty())
            {
                for (int i=0;i< outputTensor.numClasses;++i)
                {
                    m_ClassNames.push_back(std::to_string(i));
                }
            }

            m_OutputTensors.push_back(outputTensor);
        }
    }
}

void Yolo::parse_cfg_blocks_v5(const  std::vector<std::map<std::string, std::string>> &vec_block_)
{
	std::vector<float> vec_anchors;
	for (const auto &block : vec_block_)
	{
		if ("net" == block.at("type"))
		{
			assert((block.find("height") != block.end())
				&& "Missing 'height' param in network cfg");
			assert((block.find("width") != block.end()) && "Missing 'width' param in network cfg");
			assert((block.find("nc") != block.end())
				&& "Missing 'nc' param in network cfg");
			assert((block.find("depth_multiple") != block.end())
				&& "Missing 'depth_multiple' param in network cfg");
			assert((block.find("width_multiple") != block.end())
				&& "Missing 'width_multiple' param in network cfg");
			assert((block.find("anchors") != block.end())
				&& "Missing 'anchors' param in network cfg");
			assert((block.find("channels") != block.end())
				&& "Missing 'channels' param in network cfg");

			m_InputH = std::stoul(trim(block.at("height")));
			m_InputW = std::stoul(trim(block.at("width")));
			m_InputC = std::stoul(trim(block.at("channels")));
			_f_depth_multiple = std::stof(trim(block.at("depth_multiple")));
			_f_width_multiple = std::stof(trim(block.at("width_multiple")));
			_n_classes = std::stoi(trim(block.at("nc")));
			m_InputSize = m_InputC * m_InputH * m_InputW;
			std::string anchorString = block.at("anchors");
			while (!anchorString.empty())
			{
				int npos = anchorString.find_first_of(',');
				if (npos != -1)
				{
					float anchor = std::stof(trim(anchorString.substr(0, npos)));
					vec_anchors.push_back(anchor);
					anchorString.erase(0, npos + 1);
				}
				else
				{
					float anchor = std::stof(trim(anchorString));
					vec_anchors.push_back(anchor);
					break;
				}
			}
		}
		else if ("Detect" == block.at("type"))
		{
			assert((block.find("from") != block.end())
				&& "Missing 'from' param in network cfg");
			std::string from = block.at("from");
			std::vector<int> vec_from{};
			while (!from.empty())
			{
				int npos = from.find_first_of(",");
				if (-1 != npos)
				{
					vec_from.push_back(std::stoi(trim(from.substr(0, npos))));
					from.erase(0, npos + 1);
				}
				else
				{
					vec_from.push_back(std::stoi(trim(from)));
					break;
				}
			}

			for (uint32_t i = 0; i < vec_from.size(); ++i)
			{
				TensorInfo outputTensor;
				outputTensor.anchors = vec_anchors;
				outputTensor.masks = std::vector<uint32_t>{3*i,3*i+1,3*i+2};
				outputTensor.numBBoxes = outputTensor.masks.size();
				outputTensor.numClasses = _n_classes;

				m_OutputTensors.push_back(outputTensor);

				if (m_ClassNames.empty())
				{
					for (int i = 0; i < outputTensor.numClasses; ++i)
					{
						m_ClassNames.push_back(std::to_string(i));
					}
				}
			}
			
		}
	}
}
void Yolo::allocateBuffers()
{
    m_DeviceBuffers.resize(m_Engine->getNbBindings(), nullptr);
    assert(m_InputBindingIndex != -1 && "Invalid input binding index");
    NV_CUDA_CHECK(cudaMalloc(&m_DeviceBuffers.at(m_InputBindingIndex),
                             m_BatchSize * m_InputSize * sizeof(float)));

    for (auto& tensor : m_OutputTensors)
    {
        tensor.bindingIndex = m_Engine->getBindingIndex(tensor.blobName.c_str());
        assert((tensor.bindingIndex != -1) && "Invalid output binding index");
        NV_CUDA_CHECK(cudaMalloc(&m_DeviceBuffers.at(tensor.bindingIndex),
                                 m_BatchSize * tensor.volume * sizeof(float)));
        NV_CUDA_CHECK(
            cudaMallocHost(&tensor.hostBuffer, tensor.volume * m_BatchSize * sizeof(float)));
    }
}

bool Yolo::verifyYoloEngine()
{
    assert((m_Engine->getNbBindings() == (1 + m_OutputTensors.size())
            && "Binding info doesn't match between cfg and engine file \n"));

    for (auto tensor : m_OutputTensors)
    {
        assert(!strcmp(m_Engine->getBindingName(tensor.bindingIndex), tensor.blobName.c_str())
               && "Blobs names dont match between cfg and engine file \n");
        assert(get3DTensorVolume(m_Engine->getBindingDimensions(tensor.bindingIndex))
                   == tensor.volume
               && "Tensor volumes dont match between cfg and engine file \n");
    }

    assert(m_Engine->bindingIsInput(m_InputBindingIndex) && "Incorrect input binding index \n");
    assert(m_Engine->getBindingName(m_InputBindingIndex) == m_InputBlobName
           && "Input blob name doesn't match between config and engine file");
    assert(get3DTensorVolume(m_Engine->getBindingDimensions(m_InputBindingIndex)) == m_InputSize);
    return true;
}

void Yolo::destroyNetworkUtils(std::vector<nvinfer1::Weights>& trtWeights)
{
    if (m_Network) m_Network->destroy();
    if (m_Engine) m_Engine->destroy();
    if (m_Builder) m_Builder->destroy();
    if (m_ModelStream) m_ModelStream->destroy();

    // deallocate the weights
    for (uint32_t i = 0; i < trtWeights.size(); ++i)
    {
        if (trtWeights[i].count > 0) free(const_cast<void*>(trtWeights[i].values));
    }
}

void Yolo::writePlanFileToDisk()
{
    std::cout << "Serializing the TensorRT Engine..." << std::endl;
    assert(m_Engine && "Invalid TensorRT Engine");
    m_ModelStream = m_Engine->serialize();
    assert(m_ModelStream && "Unable to serialize engine");
    assert(!m_EnginePath.empty() && "Enginepath is empty");

    // write data to output file
    std::stringstream gieModelStream;
    gieModelStream.seekg(0, gieModelStream.beg);
    gieModelStream.write(static_cast<const char*>(m_ModelStream->data()), m_ModelStream->size());
    std::ofstream outFile;
    outFile.open(m_EnginePath, std::ios::binary | std::ios::out);
    outFile << gieModelStream.rdbuf();
    outFile.close();

    std::cout << "Serialized plan file cached at location : " << m_EnginePath << std::endl;
}

