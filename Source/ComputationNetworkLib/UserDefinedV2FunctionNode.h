//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//
#pragma once

#include "Basics.h"
#include "ComputationNode.h"
#include "Matrix.h"
#include "CNTKLibrary.h"
#include "Utils.h"

namespace Microsoft {
    namespace MSR {
        namespace CNTK {

            template <typename ElemType>
            class OutputMultiplexerNode;

            // -----------------------------------------------------------------------
            // UserDefinedV2Function
            // Proxy ComputationNode type for a V2 user-defined custom Function, instances
            // of which can be part of a CNTK computation network.
            // The actual implementation of the operation itself is external to the CNTK engine.
            // -----------------------------------------------------------------------

            // TODO: We currently only support external nodes that cannot be part of CNTK recurrent loops
            template <class ElemType>
            class UserDefinedV2FunctionNode final : public ComputationNode<ElemType>, public MultiOutputNode<ElemType>
            {
                typedef ComputationNode<ElemType> Base; UsingComputationNodeMembersBoilerplate;
                static const std::wstring TypeName() { return L"UserDefinedV2Function"; }

                friend class OutputMultiplexerNode<ElemType>;

            public:
                UserDefinedV2FunctionNode(DEVICEID_TYPE deviceId, const wstring& name, const ::CNTK::FunctionPtr& externalFunction = nullptr)
                    : Base(deviceId, name), m_externalFunction(externalFunction), MultiOutputNode<ElemType>(externalFunction ? externalFunction->Outputs().size() : 0)
                {
                    if (!m_externalFunction)
                        LogicError("UserDefinedV2FunctionNode ctor should never be called with externalFunction == nullptr");
                }

                virtual bool ForceDynamicValidation() const override
                {
                    auto outputs = m_externalFunction->Outputs();
                    return std::any_of(outputs.begin(), outputs.end(), [](const ::CNTK::Variable& output) { return output.Shape().HasFreeDimension(); });
                }

                // This function is called in both PAR and SEQ modes of execution.
                // In PAR mode, all frames are included at once and the MBLayout of the
                // function defines the entire output.
                // In the SEQ mode, we need to call UDF with input corresponding to each
                // frame. The produced output also needs to be properly positioned in the
                // final output matrix.
                virtual void ForwardProp(const FrameRange& fr) override
                {
                    bool inSEQMode = false;
                    if (!fr.IsAllFrames())
                    {
                        inSEQMode = true;
                    }

                    // The first output value is set as this node's output. Others are mapped
                    // using OutputMultiplexerNode when creating the computation network.
                    this->m_outputsValue[0] = m_value;

                    // Get the arguments of the external function
                    auto arguments = m_externalFunction->Arguments();
                    std::unordered_map<::CNTK::Variable, ::CNTK::ValuePtr> argumentValues;
                    auto numInputs = GetNumInputs();
                    size_t j = 0;
                    for (size_t i = 0; i < numInputs; ++i)
                    {
                        auto& input = InputRef(i);
                        if (input.template Is<LearnableParameter<ElemType>>())
                            continue;

                        auto argumentVar = arguments[j++];

                        // MBLayout and the frame has to point to the correct slice of the
                        // data in the SEQ mode. For PAR mode, this function is called
                        // only once with all frames.
                        MBLayoutPtr layout = make_shared<MBLayout>();
                        FrameRange inputFr = fr;
                        if (inSEQMode)
                        {
                            layout->InitAsFrameMode(1);
                        }
                        else
                        {
                            layout = input.GetMBLayout();
                            inputFr = fr.WithLayout(input.GetMBLayout());
                        }

                        auto inputValueForFrame = input.ValueFor(inputFr);
                        auto argumentShape = ::CNTK::AsNDShape(input.GetSampleLayout());

                        // Get the argument value pointer for the provided frame.
                        auto argumentValue =
                            ::CNTK::Utils::GetValueObjectFromCNTKImplMatrixAndMBLayout(
                                argumentShape,
                                argumentVar.DynamicAxes(),
                                inputValueForFrame, // only for the particular frame.
                                layout); // layout for the frame.

                        argumentValues.insert(std::make_pair(argumentVar, argumentValue));
                    }
                    assert(j == arguments.size());

                    auto outputs = m_externalFunction->Outputs();
                    std::unordered_map<::CNTK::Variable, ::CNTK::ValuePtr> outputValues;
                    for (auto output : outputs)
                    {
                        outputValues.insert({ output, nullptr });
                    }

                    std::unordered_set<::CNTK::Variable> outputsToRetainBackwardStateFor;
                    if (Environment().IsTraining())
                        outputsToRetainBackwardStateFor.insert(outputs.begin(), outputs.end());

                    auto computeDevice = ::CNTK::AsDeviceDescriptor(InputRef(0).Value().GetDeviceId());

                    m_currentBackpropStatePtr = m_externalFunction->Forward(
                        argumentValues,
                        outputValues,
                        computeDevice,
                        outputsToRetainBackwardStateFor);

                    // Copy the computed output to MultiOutputNode node.
                    for (size_t i = 0; i < outputs.size(); ++i)
                    {
                        auto output = outputs[i];
                        ::CNTK::NDShape inferredVarShape;
                        // Call this function to retrieve the computer output matrix.
                        // The shape is based on what we have provided in the forward.
                        auto outputMatrixAndLayout =
                            ::CNTK::Utils::GetCNTKImplMatrixAndMBLayoutFromValueObject<ElemType>(
                                output,
                                outputValues[output],
                                &inferredVarShape);

                        if (inferredVarShape.IsUnknown() || inferredVarShape.HasUnboundDimension())
                            LogicError("The output shape '%S' of an external user defined Function '%S' "
                                "must be fully defined.", inferredVarShape.AsString().c_str(),
                                m_externalFunction->AsString().c_str());

                        if (output.Shape().HasFreeDimension())
                        {
                            this->m_outputsShape[i] = ::CNTK::AsTensorShape(inferredVarShape);
                            if (i == 0)
                                SetDims(this->m_outputsShape[i], HasMBLayout());
                        }

                        if (inSEQMode)
                        {
                            // Replace only a column of the output value corresponding to the
                            // input frame.
                            this->m_outputsValue[i]->SetColumn(*outputMatrixAndLayout.first, fr.timeIdxInSeq);
                        }
                        else
                        {
                            // Set the entire output value.
                            this->m_outputsValue[i]->SetValue(*outputMatrixAndLayout.first);
                        }

                        if ((this->m_outputsMBLayout[i] != nullptr) && (outputMatrixAndLayout.second == nullptr))
                            LogicError("The UserDefinedFunction node has a non-null output MBLayout but none found from the '%S' user Function::Forward output Value", m_externalFunction->Name().c_str());
                        else if ((this->m_outputsMBLayout[i] == nullptr) && (outputMatrixAndLayout.second != nullptr))
                            LogicError("The UserDefinedFunction node does not have an output MBLayout but the '%S' user Function::Forward output Value has a non-null layout", m_externalFunction->Name().c_str());
                        else if ((this->m_outputsMBLayout[i] == nullptr) && (outputMatrixAndLayout.second == nullptr))
                            ;
                        else if (!inSEQMode)
                        {
                            if (this->m_outputsHasNewMBLayout[i])
                            {
                                // In SEQMode the layout has to be fixed after validation.
                                this->m_outputsMBLayout[i]->CopyFrom(outputMatrixAndLayout.second);
                            }
                            else
                            {
                                if (*this->m_outputsMBLayout[i] != *outputMatrixAndLayout.second)
                                    LogicError("The MBLayout 'NumSequences=%zu, NumTimeSteps=%zu' of the output computed by the external function '%S' does not match the expected MBLayout 'NumSequences=%zu, NumTimeSteps=%zu'.",
                                        outputMatrixAndLayout.second->GetNumSequences(), outputMatrixAndLayout.second->GetNumTimeSteps(),
                                        m_externalFunction->Name().c_str(),
                                        this->m_outputsMBLayout[i]->GetNumSequences(), this->m_outputsMBLayout[i]->GetNumTimeSteps());
                            }
                        }
                    }
                }

                // Similar to forward, this function also getting called from both PAR and
                // SEQ modes of execution. Here we need to get the gradient corresponding 
                // to the frame and place it in the proper location in the SEQ mode.
                // PAR Mode is a single invocation for the whole gradient matrix.
                virtual void BackpropTo(const size_t inputIndex, const FrameRange& fr) override
                {
                    if (m_currentBackpropStatePtr == nullptr)
                        return;

                    bool inSEQMode = false;
                    if (!fr.IsAllFrames())
                    {
                        inSEQMode = true;
                    }

                    // Similar to the output, the gradient 0 is set to this node's
                    // gradient. other values are handled by OutputMultiplexerNode.
                    this->m_outputsGradient[0] = m_gradient;

                    std::unordered_map<::CNTK::Variable, ::CNTK::ValuePtr> outputGradientValues;
                    auto outputs = m_externalFunction->Outputs();
                    bool noOutputNeedsGradient = std::all_of(outputs.begin(), outputs.end(), [](const ::CNTK::Variable& outVar) { return !outVar.NeedsGradient(); });
                    if (noOutputNeedsGradient)
                        return;

                    for (size_t i = 0; i < outputs.size(); ++i)
                    {
                        auto output = outputs[i];

                        // MBLayout and the frame has to point to the correct slice of the
                        // data in the SEQ mode. For PAR mode, this function is called
                        // only once with all frames.
                        MBLayoutPtr layout = make_shared<MBLayout>();
                        std::shared_ptr<Matrix<ElemType>> outputGradient;
                        if (inSEQMode)
                        {
                            layout->InitAsFrameMode(1);
                            outputGradient = std::make_shared<Matrix<ElemType>>(this->m_outputsGradient[i]->ColumnSlice(fr.timeIdxInSeq, 1));
                        }
                        else
                        {
                            layout = this->m_outputsMBLayout[i];
                            outputGradient = this->m_outputsGradient[i];
                        }

                        // TODO: We unpack the same output gradients each time this method is called for a different input.
                        // We should be able to cache the unpacked values during backpropagation of gradients to the first
                        // input, and reuse them for subsequence inputs.
                        ::CNTK::ValuePtr gradientValue;
                        if (output.NeedsGradient())
                            gradientValue =
                            ::CNTK::Utils::GetValueObjectFromCNTKImplMatrixAndMBLayout(
                                ::CNTK::AsNDShape(this->m_outputsShape[i]),
                                output.DynamicAxes(),
                                *outputGradient,
                                layout);

                        outputGradientValues.insert({ output, gradientValue });
                    }

                    std::vector<::CNTK::Variable> externalFunctionUniqueInputs;
                    auto externalFunctionInputs = m_externalFunction->Inputs();
                    for (auto input : externalFunctionInputs)
                    {
                        if (std::find(externalFunctionUniqueInputs.begin(), externalFunctionUniqueInputs.end(), input) == externalFunctionUniqueInputs.end())
                            externalFunctionUniqueInputs.push_back(input);
                    }

                    std::unordered_map<::CNTK::Variable, ::CNTK::ValuePtr> inputGradientValues;
                    for (size_t i = 0; i < externalFunctionUniqueInputs.size(); ++i)
                    {
                        //This is a BUGBUG i is not the same once we put into unique
                        if (InputRef(i).NeedsGradient())
                            inputGradientValues.insert({ externalFunctionUniqueInputs[i], nullptr });
                    }

                    m_externalFunction->Backward(m_currentBackpropStatePtr, outputGradientValues, inputGradientValues);

                    // Accumulate the computed input gradient value into the existing input gradient value
                    // TODO: We should directly pass the actual input gradient tensor to the Backward method 
                    // instead of allocating a new value and accumulating it ourselves
                    for (size_t i = 0; i < externalFunctionUniqueInputs.size(); ++i)
                    {
                        if (!InputRef(i).NeedsGradient())
                            continue;

                        InputRef(i).LazyZeroGradient(this); // set gradient to 0 if this is the first time

                        auto input = externalFunctionUniqueInputs[i];
                        auto inputGradientValue = inputGradientValues[input];
                        if (!inputGradientValue)
                            continue;

                        // Get the input gradient for the particular input.
                        auto newInputGradientMatrixAndLayout =
                            ::CNTK::Utils::GetCNTKImplMatrixAndMBLayoutFromValueObject<ElemType>(
                                input,
                                inputGradientValue);

                        // Set the gradient based on the current frame.
                        auto& inputNode = InputRef(i);
                        if (inputNode.HasMBLayout() && inSEQMode)
                        {
                            inputNode.GradientFor(fr) += *newInputGradientMatrixAndLayout.first;
                        }
                        else
                        {
                            inputNode.Gradient() += *newInputGradientMatrixAndLayout.first;

                            if (*InputRef(i).GetMBLayout() != *newInputGradientMatrixAndLayout.second)
                                LogicError("The MBLayout 'NumSequences=%zu, NumTimeSteps=%zu' of the Input(%zu) gradient computed by the external function '%S' does not match the expected MBLayout 'NumSequences=%zu, NumTimeSteps=%zu'.",
                                    newInputGradientMatrixAndLayout.second->GetNumSequences(), newInputGradientMatrixAndLayout.second->GetNumTimeSteps(),
                                    i, this->GetName().c_str(),
                                    InputRef(i).GetMBLayout()->GetNumSequences(), InputRef(i).GetMBLayout()->GetNumTimeSteps());
                        }
                    }

                    // Set the back-prop state to null when the last time frame
                    // (actually the first due to backward calling) is executed.
                    if (!inSEQMode || fr.timeIdxInSeq == 0)
                    {
                        m_currentBackpropStatePtr = nullptr;
                    }
                }

                virtual void Validate(bool isFinalValidationPass) override
                {
                    Base::Validate(isFinalValidationPass);

                    // For UDF we need to infer the MBLayout for the function.
                    // The following code, will find the first output that has 
                    // dynamic axes similar to one of the inputs and use the
                    // MBLayout of that input as the UDF's MBLayout.

                    auto outputs = m_externalFunction->Outputs();
                    bool layoutNotInitialized = (m_pMBLayout == nullptr);

                    if (layoutNotInitialized)
                    {
                        bool matchingDynamicAxesFound = false;
                        int matchCount;

                        auto arguments = m_externalFunction->Arguments();
                        for (size_t i = 0; i < outputs.size() && !matchingDynamicAxesFound; ++i)
                        {
                            auto output = outputs[i];
                            for (size_t j = 0; j < arguments.size(); ++j)
                            {
                                if (!m_inputs[j]->HasMBLayout())
                                {
                                    continue;
                                }

                                auto inputDynamicAxes = arguments[j].DynamicAxes();
                                auto outputDynamicAxes = output.DynamicAxes();

                                // The number of output dynamic axes should be equal or less
                                // than the input dynamic axes.
                                if (outputDynamicAxes.size() > inputDynamicAxes.size())
                                {
                                    continue;
                                }

                                matchCount = 0;
                                for (size_t k = 0; k < outputDynamicAxes.size(); ++k)
                                {
                                    if (inputDynamicAxes[k] == outputDynamicAxes[k])
                                    {
                                        ++matchCount;
                                    }
                                }

                                if (matchCount == outputDynamicAxes.size())
                                {
                                    assert(m_inputs.size() >= j); // one to one mapping between inputs and arguments?
                                    LinkToMBLayout(InputRef(j).GetMBLayout());
                                    matchingDynamicAxesFound = true;
                                    break;
                                }
                            }
                        }

                        if (!matchingDynamicAxesFound)
                        {
                            InferMBLayoutFromInputsForStandardCase(isFinalValidationPass);
                        }
                    }

                    for (size_t i = 0; i < outputs.size(); ++i)
                    {
                        auto output = outputs[i];

                        if (output.GetDataType() != ::CNTK::AsDataType<ElemType>())
                        {
                            LogicError("The DataType '%s' of the external user defined Function's output does not match the internal ComputationNode's ElemType '%s'.",
                                DataTypeName(output.GetDataType()),
                                DataTypeName(::CNTK::AsDataType<ElemType>()));
                        }

                        m_outputsMBLayout[i] = m_pMBLayout;
                        if (layoutNotInitialized)
                        {
                            this->m_outputsHasNewMBLayout[i] = true;
                        }

                        auto outputNDShape = output.Shape();
                        for (size_t k = 0; k < outputNDShape.Rank(); ++k)
                        {
                            if ((outputNDShape[k] == ::CNTK::NDShape::FreeDimension) || (outputNDShape[k] == ::CNTK::NDShape::InferredDimension))
                                outputNDShape[k] = 1;
                        }

                        this->m_outputsShape[i] = ::CNTK::AsTensorShape(outputNDShape);
                        SetDims(this->m_outputsShape[i], HasMBLayout());
                    }
                }

            private:
                ::CNTK::FunctionPtr m_externalFunction;
                ::CNTK::BackPropStatePtr m_currentBackpropStatePtr;
            };

            template class UserDefinedV2FunctionNode<float>;
            template class UserDefinedV2FunctionNode<double>;

        }
    }
}
