#ifndef CXXNET_NET_INL_HPP
#define CXXNET_NET_INL_HPP
#pragma once
/*!
 * \file cxxnet_net-inl.hpp
 * \brief implementation of netural nets
 * \author Tianqi Chen, Bing Xu
 */
#include "cxxnet_nnet.h"
#include "../core/cxxnet_core.h"
#include "../utils/cxxnet_utils.h"
#include "../utils/cxxnet_metric.h"

namespace cxxnet {
    using namespace mshadow::utils;
    using namespace mshadow::expr;

    /*! \brief data structure that contains general shape of network */
    struct NetMetaModel{
    public:
        /*! \brief general model parameter */
        struct Param{
            /*! \brief number of nodes in the network */
            int num_nodes;
            /*! \brief number of layers in the network */
            int num_layers;
            /*! \brief input shape, not including batch dimension */
            mshadow::Shape<3> shape_in;
            /*! \brief whether the network is initialized */
            int init_end;
            /*! \brief number of epoches pass so far */
            int64_t num_epoch_passed;
            /*! \brief reserved flag, used to extend data structure */
            int reserved_flag;
            /*! \brief constructor, reserved flag */
            Param( void ){
                init_end = 0;
                num_epoch_passed = 0;
                reserved_flag = 0;
            }
            /*! \brief get input shape, given number of batches */
            mshadow::Shape<4> GetShapeIn( index_t nbatch ) const{
                if( shape_in[2] == 1 && shape_in[1] == 1 ){
                    return mshadow::Shape4( 1, 1, nbatch, shape_in[0] );
                }else{
                    return mshadow::Shape4( nbatch, shape_in[2], shape_in[1], shape_in[0] );
                }
            }
        };
        /*! \brief information about each layer */
        struct LayerInfo{
            /*! \brief type of layer */
            int type;
            /*! \brief input node index */
            int nindex_in;
            /*! \brief output node index */
            int nindex_out;
            inline bool operator==( const LayerInfo &b ) const{
                return type == b.type && nindex_in == b.nindex_in && nindex_out == b.nindex_out;
            }
        };
    public:
        /*! \brief model parameter */
        Param param;
        /*! \brief information about each layers */
        std::vector<LayerInfo> layers;
    public:
        /*! \brief set model parameters */
        inline void SetParam( const char *name, const char *val ){
            if( !strcmp( name, "reset_epoch") ) param.num_epoch_passed = (int64_t)atol( val );
            if( param.init_end != 0 ) return;
            if( !strcmp( name, "input_shape") ){
                unsigned x, y, z;
                Assert( sscanf( val, "%u,%u,%u", &z,&y,&x ) ==3,
                               "input_shape must be three consecutive integers without space example: 1,1,200 " );
                param.shape_in[0] = x; param.shape_in[1] = y; param.shape_in[2] = z;
            }
        }
        /*! \brief guess parameters, from current setting */
        inline void InitModel( void ){
            param.num_nodes = 0;
            param.num_layers = static_cast<int>( layers.size() );
            for( size_t i = 0; i < layers.size(); ++ i ){
                param.num_nodes = std::max( layers[i].nindex_out + 1, param.num_nodes );
            }
            param.init_end = 1;
        }
        virtual void SaveModel( mshadow::utils::IStream &fo ) const {
            fo.Write( &param, sizeof( Param ) );
            fo.Write( &layers[0], sizeof(LayerInfo) * layers.size() );
        }
        virtual void LoadModel( mshadow::utils::IStream &fi ) {
            Assert( fi.Read( &param, sizeof( Param ) ) != 0 );
            layers.resize( param.num_layers );
            if( layers.size() != 0 ){
                Assert( fi.Read( &layers[0], sizeof(LayerInfo) * layers.size() ) != 0 );
            }
        }
        inline int LastNode(void) const{
            if( layers.size() == 0 ) return 0;
            else return layers.back().nindex_out;
        }
    };

    /*! \brief helper class to config networks */
    struct NetConfigHelper{
    public:
        NetConfigHelper( NetMetaModel &meta ):meta(meta){
            this->netcfg_mode = 0;
            this->updater_type = "sgd";
            this->batch_size   = 100;
        }
        // set parameters
        inline void SetParam( const char *name, const char *val ){
            meta.SetParam( name, val );
            if( !strcmp( name, "batch_size" ) ) batch_size = atoi( val );
            if( !strcmp( name, "updater" ) )    updater_type = val;
            if( !strcmp( name, "netconfig" ) && !strcmp( val, "start") ) netcfg_mode = 1;
            if( !strcmp( name, "netconfig" ) && !strcmp( val, "end") )   netcfg_mode = 0;

            if( !strncmp( name, "layer[", 6 ) ){
                netcfg_mode = 2;
                if( meta.param.init_end == 0 ){
                    meta.layers.push_back( this->GetLayerInfo( name, val, meta.LastNode() ) );
                    meta.param.num_layers = static_cast<int>( meta.layers.size() );
                }
            }
            if( netcfg_mode == 2 ){
                // layer specific configuration
                netcfg.push_back( std::make_pair( std::string(name), std::string(val) ) );
            }else{
                defcfg.push_back( std::make_pair( std::string(name), std::string(val) ) );
            }
        }
        template<typename xpu>
        inline void ConfigLayers( std::vector< Node<xpu> >& nodes,
                                  std::vector<ILayer*>& layers,
                                  std::vector<IUpdater*>& updaters, bool init_model ){
            // default configuration
            int layer_index = -1;
            int top_node = 0;
            for( size_t i = 0; i < netcfg.size(); ++i ){
                const char* name = netcfg[i].first.c_str();
                const char* val  = netcfg[i].second.c_str();
                if( !strncmp( name, "layer[", 6 ) ){
                    ++ layer_index;
                    Assert( layer_index >= 0 && layer_index < meta.param.num_layers );

                    NetMetaModel::LayerInfo inf = this->GetLayerInfo( name, val, top_node );
                    top_node = inf.nindex_out;

                    Assert( inf == meta.layers[layer_index], "config setting mismatch" );
                    // set global parameters
                    for( size_t j = 0; j < defcfg.size(); ++ j ){
                        layers[ layer_index ]->
                            SetParam( defcfg[j].first.c_str(), defcfg[j].second.c_str() );
                    }
                }else{
                    Assert( layer_index >= 0 );
                    layers[ layer_index ]->SetParam( name, val );
                }
            }
            // adjust node Shape
            nodes[0].data.shape = meta.param.GetShapeIn( batch_size );
            for( size_t i = 0; i < layers.size(); ++i ){
                layers[i]->InitLayer();
                if( init_model ) layers[i]->InitModel();
            }
            // configure updaters
            layer_index = -1;
            size_t ustart = 0;

            for( size_t i = 0; i < netcfg.size(); ++i ){
                const char* name = netcfg[i].first.c_str();
                const char* val  = netcfg[i].second.c_str();
                if( !strncmp( name, "layer[", 6 ) ){
                    ++ layer_index;
                    ustart = updaters.size();
                    layers[ layer_index ]->GetUpdaters( updater_type.c_str(), updaters );
                    for( size_t j = ustart; j < updaters.size(); ++ j ){
                        for( size_t k = 0; k < defcfg.size(); ++ k ){
                            updaters[j]->
                                SetParam( defcfg[k].first.c_str(), defcfg[k].second.c_str() );
                        }
                    }
                }else{
                    Assert( layer_index >= 0 );
                    for( size_t j = ustart; j < updaters.size(); ++ j ){
                        updaters[j]->SetParam( name, val );
                    }
                }
            }
            for( size_t i = 0; i < updaters.size(); ++ i ){
                updaters[i]->Init();
            }
        }
    private:
        inline NetMetaModel::LayerInfo GetLayerInfo( const char *name, const char *val, int top_node ){
            int a, b;
            char ltype[256],tag[256];
            if( sscanf( name, "layer[%d->%d]", &a, &b ) != 2 ){                
                utils::Assert( sscanf( name, "layer[+%d]", &b ) == 1, 
                               "invalid config format, correct example: layer[1->2]" );
                a = top_node; b += top_node;
            }
            Assert( sscanf( val , "%[^:]:%s", ltype, tag ) >= 1, "invalid config format" );
            NetMetaModel::LayerInfo inf;
            inf.nindex_in = a; inf.nindex_out = b;
            inf.type = GetLayerType( ltype );
            return inf;
        }
    private:
        NetMetaModel &meta;
        // type of updater
        std::string updater_type;
        // configures about network
        std::vector< std::pair< std::string, std::string > > netcfg, defcfg;
        // number of batch size
        int batch_size;
        // whether in net config mode
        int netcfg_mode;
    };

    /*!
     * \brief data structure of netural net
     * \tparam xpu data storage type
     */
    template<typename xpu>
    struct NeuralNet{
    public:
        /*!\brief do not print anything */
        int silent;
        /*! \brief meta information about network */
        NetMetaModel meta;
        /*! \brief configure helper */
        NetConfigHelper cfg;
        /*! \brief nodes in the neural net */
        std::vector< Node<xpu> > nodes;
        /*! \brief layers in the neural net */
        std::vector<ILayer*>     layers;
        /*! \brief updaters in the neural net */
        std::vector<IUpdater*>   updaters;
        /*! \brief random number generator */
        mshadow::Random<xpu>     rnd;
        /*! \brief reserved  cpu random number generator */
        mshadow::Random<cpu>     rnd_cpu;
        /*! \brief node factory */
        NodeFactory<xpu> nfactory;
        /*! \brief temp space */
        mshadow::TensorContainer<cpu,2> temp;
    public:
        /*! \brief constructor */
        NeuralNet( void ): cfg(meta),rnd(0), rnd_cpu(0) {
            silent = 0;
        }
        /*! \brief destructor */
        ~NeuralNet( void ){
            this->FreeSpace();
        }
        /*! \brief input node */
        inline Node<xpu>& in( void ){
            return nodes[0];
        }
        /*! \brief output node */
        inline Node<xpu>& out( void ){
            return nodes.back();
        }
        /*! \brief set parameter */
        inline void SetParam( const char *name, const char *val ){
            if( !strcmp( name, "seed") ){
                rnd.Seed( atoi( val ) ); rnd_cpu.Seed( atoi(val) );
            }
            if( !strcmp( name, "silent") ) silent = atoi(val);
            if( !strcmp( name, "memlimit") ) nfactory.SetMemLimit( val );
            cfg.SetParam( name, val );
        }
        /*! \brief intialize model parameters */
        inline void InitModel( void ) {
            this->FreeSpace();
            meta.InitModel();
            for( int i = 0; i < meta.param.num_nodes; ++i ){
                nodes.push_back( nfactory.CreateNode() );
            }
            for( int i = 0; i < meta.param.num_layers; ++ i ){
                Assert( layers.size() == (size_t) i );
                const NetMetaModel::LayerInfo &info = meta.layers[i];
                layers.push_back( CreateLayer( info.type, rnd, nodes[ info.nindex_in ], nodes[ info.nindex_out ] ) );
            }
            cfg.ConfigLayers( nodes, layers, updaters, true );
            this->InitNodes();
        }
        /*! \brief save model to file */
        inline void SaveModel( mshadow::utils::IStream &fo ) const {
            meta.SaveModel( fo );
            for( int i = 0; i < meta.param.num_layers; ++ i ){
                layers[i]->SaveModel( fo );
            }
        }
        /*! \brief load model from stream */
        inline void LoadModel( mshadow::utils::IStream &fi ) {
            this->FreeSpace();
            meta.LoadModel( fi );
            for( int i = 0; i < meta.param.num_nodes; ++i ){
                nodes.push_back( nfactory.CreateNode() );
            }
            for( int i = 0; i < meta.param.num_layers; ++ i ){
                const NetMetaModel::LayerInfo &info = meta.layers[i];
                Assert( layers.size() == (size_t) i );
                layers.push_back ( CreateLayer( info.type, rnd, nodes[ info.nindex_in ], nodes[ info.nindex_out ] ) );
                layers[i]->LoadModel( fi );
            }
            cfg.ConfigLayers( nodes, layers, updaters, false );
            this->InitNodes();
        }
        /*!
         * \brief forward prop
         * \param is_train whether is training phase
         */
        inline void Forward( bool is_train ){
            for( size_t i = 0; i < layers.size(); ++ i ){
                layers[i]->Forward( is_train );
            }
        }
        /*!
         * \brief inference forward prop
         * \param stop_layer
         */
        inline void Inference(int stop_layer, long total_length, int &header_flag, mshadow::utils::IStream &fo) {
            utils::Assert(stop_layer < layers.size() && stop_layer > 0, "Incorrect stop layer");
            for (size_t i = 0; i < stop_layer + 1; ++i) {
                layers[i]->Forward(false);
            }
            Assert( nodes[stop_layer].is_mat() );
            if (header_flag == 0) {
                header_flag = 1;
                // Write total length and feature
                fo.Write(&total_length, sizeof(long));
                fo.Write(&(nodes[stop_layer].data.shape[0]), sizeof(mshadow::index_t));
            }
            nodes[stop_layer].Pin();
            mshadow::Tensor<cpu, 4> tmp(nodes[stop_layer].data.shape);
            mshadow::AllocSpace(tmp);
            mshadow::Copy(tmp, nodes[stop_layer].data);
            for (int i = 0; i < tmp.shape[1]; ++i) {
                for (int j = 0; j < tmp.shape[0]; ++j) {
                    fo.Write(&(tmp[0][0][i][j]), sizeof(mshadow::real_t));
                }
            }
            mshadow::FreeSpace(tmp);
            nodes[stop_layer].Unpin();
        }
        /*! \brief backprop */
        inline void Backprop( bool prop_to_firstlayer = false ){
            for( size_t i = layers.size(); i > 0; -- i ){
                layers[i-1]->Backprop( i != 1 || prop_to_firstlayer );
            }
        }
        /*! \brief update model parameters  */
        inline void Update( void ){
            for( size_t i = 0; i < updaters.size(); ++ i ){
                updaters[i]->Update( meta.param.num_epoch_passed );
            }
            // update epoch
            meta.param.num_epoch_passed += 1;
        }
        /*!
         * \brief notify round start
         * \param round round counter
         */
        virtual void StartRound( int round ) {
            for( size_t i = 0; i < updaters.size(); ++ i ){
                updaters[i]->StartRound( round );
            }
        }
    private:
        /*! \brief check the node shapes */
        inline void InitNodes( void ){
            for( size_t i = 0; i < nodes.size(); ++ i ){
                mshadow::Shape<4> s = nodes[i].data.shape;
                nodes[i].Pin(); nodes[i].Unpin();
                if( !silent ){
                    printf("node[%d].shape: %u,%u,%u,%u\n",(int)i, s[3],s[2],s[1],s[0] );
                }
            }
        }
        /*! \brief set parameters */
        inline void FreeSpace( void ){
            for( size_t i = 0; i < nodes.size(); ++ i ){
                nodes[i].FreeSpace();
            }
            for( size_t i = 0; i < layers.size(); ++ i ){
                delete layers[i];
            }
            for( size_t i = 0; i < updaters.size(); ++ i ){
                delete updaters[i];
            }
            nodes.clear(); layers.clear(); updaters.clear();
        }
    };

    /*! \brief implementation of neural network trainer */
    template<typename xpu>
    class CXXNetTrainer : public INetTrainer{
    public:
        CXXNetTrainer( void ){
            loss_type = 0; round = 0;
            update_period = 1;
            sample_counter = 0;
            eval_train = 1;
            if( net.silent == 0 ){
                printf("CXXNetTrainer, devCPU=%d\n", xpu::kDevCPU );
            }
        }
        virtual ~CXXNetTrainer( void ){
        }
        virtual void SetParam( const char *name, const char *val ){
            if( !strcmp( name, "loss" ) )  loss_type = atoi( val );
            if( !strcmp( name, "update_period" ) )  update_period = atoi( val );
            if( !strcmp( name, "metric") ) {
                metric.AddMetric( val ); train_metric.AddMetric(val);
            }
            if( !strcmp( name, "eval_train")) eval_train = atoi(val);
            net.SetParam( name, val );
        }
        virtual void InitModel( void ) {
            net.InitModel();
        }
        virtual void SaveModel( mshadow::utils::IStream &fo ) const {
            net.SaveModel( fo );
        }
        virtual void LoadModel( mshadow::utils::IStream &fi ) {
            net.LoadModel( fi );
        }
        // tell trainer which round it is
        virtual void StartRound( int round ) {
            net.StartRound( round );
            this->round = round;
        }
        virtual void ForwardBackprop ( const DataBatch& batch, bool prop_to_firstlayer ) {
            this->MakeInput( batch );

            net.Forward( true );
            this->SyncOuput();

            net.out().Pin();
            mshadow::Copy( net.out().data[0][0], temp );
            net.out().Unpin();

            this->SetLoss( batch.labels );
            net.Backprop( prop_to_firstlayer );
            if( ++ sample_counter >= update_period ){
                net.Update(); sample_counter = 0;
            }
        }        
        virtual void Update ( const DataBatch& batch ) {
            this->ForwardBackprop( batch, false );
        }
        
        virtual std::string Evaluate( IIterator<DataBatch> *iter_eval, const char* evname ){          
            std::string res;            
            if (eval_train != 0 ) {
                res += train_metric.Print("train");
                train_metric.Clear();
            }
            
            if( iter_eval == NULL ) return res;
            
            metric.Clear();
            iter_eval->BeforeFirst();
            while( iter_eval->Next() ){
                const DataBatch& batch = iter_eval->Value();
                this->PreparePredTemp( batch );
                metric.AddEval( temp.Slice(0, temp.shape[1]-batch.num_batch_padd), batch.labels );
            }

            res += metric.Print( evname );

            return res;
        }
     
        virtual void Predict( std::vector<float> &preds, const DataBatch& batch ) {
            this->PreparePredTemp( batch );
            for( index_t i = 0; i <temp.shape[1]; ++i ){
                preds.push_back( this->TransformPred( temp[i] ) );
            }
        }
        virtual void Inference(int layer, const DataBatch& batch, long total_length, int &header_flag, mshadow::utils::IStream &fo) {
            this->MakeInput(batch);
            net.Inference(layer, total_length, header_flag, fo);
        }
    protected:
        // given existing batch, make input layer parameters
        virtual void MakeInput( const DataBatch& batch ){
            net.in().Pin();
            mshadow::Copy( net.in().data, batch.data );
            net.in().Unpin();
        }
        // put prediction into temp
        virtual void PreparePredTemp( const DataBatch& batch ){
            this->MakeInput(batch);
            net.Forward( false );
            this->SyncOuput();
        }
    private:
        inline void SyncOuput( void ){
            mshadow::Shape<4> oshape  = net.out().data.shape;
            Assert( net.out().is_mat() );
            temp.Resize( mshadow::Shape2( oshape[1], oshape[0] ) );
            net.out().Pin();
            mshadow::Copy( temp, net.out().data[0][0] );
            net.out().Unpin();
        }
        inline float TransformPred( mshadow::Tensor<cpu,1> pred ){
            switch( loss_type ){
            case 0: return GetMaxIndex( pred );
            case 1: return pred[0];
            case 2: return 1.0f/(1.0f+std::exp(-pred[0]));
            default: Error("unknown loss type"); return 0.0f;
            }
        }
        inline void SetLoss( mshadow::Tensor<cpu,1> pred, float label ){
            switch( loss_type ){
            case 0:{
                index_t k = static_cast<index_t>(label);
                utils::Assert( k < pred.shape[0], "label exceed output bound" );
                pred[ k ] -= 1.0f; break;
            }
            case 1: pred[ 0 ] -=  label; break;
            case 2: pred[ 0 ] = 1.0f/(1.0f+std::exp(-pred[0])) - label; break;
            default: Error("unknown loss type");
            }
        }
        inline void SetLoss( const float* labels ){
            if( loss_type == 1 || loss_type == 2 ){
                Assert( temp.shape[0] == 1, "regression can only have 1 output size" );
            }
            // evlauate training loss
            if (eval_train != 0) {
                train_metric.AddEval(temp, labels);
            }
            for( index_t i = 0; i <temp.shape[1]; ++i ){
                this->SetLoss( temp[i], labels[i] );
            }
            net.out().Pin();
            mshadow::Copy( net.out().data[0][0], temp );
            // scale by batch_size
            net.out().data *= 1.0f / ( temp.shape[1] * update_period );
            net.out().Unpin();
        }
        inline static int GetMaxIndex( mshadow::Tensor<cpu,1> pred ){
            index_t maxidx = 0;
            for( index_t i = 1; i < pred.shape[0]; ++ i ){
                if( pred[i] > pred[maxidx] ) maxidx = i;
            }
            return maxidx;
        }
    protected:
        /*! \brief current round */
        int round;
        /*! \brief loss function */
        int loss_type;
        /*! \brief update period */
        int update_period;
        /*! \brief sample counter */
        int sample_counter;
        /*! \brief evaluator */
        utils::MetricSet metric;
        /*! \brief temp space */
        mshadow::TensorContainer<cpu,2> temp;
        /*! \brief true net */
        NeuralNet<xpu> net;
        /*! \brief tmp stoage of top index */
        std::vector<index_t> tmp_index_;
        /*! \brief show train eval */
        int eval_train;
        /*! \brief evaluator for train */
        utils::MetricSet train_metric;
    }; // class NeuralNet


    /*!
     * \brief implementation of averaging neural network trainer
     *        will perform weight averaging during predictions
     */
    template<typename xpu>
    class CXXAvgNetTrainer: public CXXNetTrainer<xpu>{
    public:
        CXXAvgNetTrainer( void ){
            num_burn = INT_MAX;
            num_avg_record = 0;
        }
        virtual ~CXXAvgNetTrainer( void ){}
        virtual void SetParam( const char *name, const char *val ){
            CXXNetTrainer<xpu>::SetParam( name, val );
            if( !strcmp( "num_inst",name) ) num_avg_record = atoi(val);
            if( !strcmp( "num_burn",name) ) num_burn = atoi(val);
        }
        virtual void InitModel( void ){
            CXXNetTrainer<xpu>::InitModel();
            this->InitAvgRecord();
        }
        virtual void SaveModel( mshadow::utils::IStream &fo ) const {
            CXXNetTrainer<xpu>::SaveModel( fo );
            fo.Write( &num_avg_record, sizeof(int) );
            fo.Write( &ref_counter[0], ref_counter.size() * sizeof(int) );
            avg_pred.SaveBinary( fo );
        }
        virtual void LoadModel( mshadow::utils::IStream &fi ) {
            CXXNetTrainer<xpu>::LoadModel( fi );
            if( this->net.meta.param.reserved_flag != 0 ){
                Assert( fi.Read( &num_avg_record, sizeof(int) )!= 0 );
                ref_counter.resize( num_avg_record );
                Assert( fi.Read( &ref_counter[0], ref_counter.size() * sizeof(int) ) != 0 );
                avg_pred.LoadBinary( fi );
            }else{
                this->InitAvgRecord();
                if( this->net.silent == 0 ){
                    printf("CXXNetAvgTrainer: init load from CXXNetTrainer model\n");
                }
            }
        }
    protected:
        virtual void PreparePredTemp( const DataBatch& batch ){
            CXXNetTrainer<xpu>::PreparePredTemp( batch );
            mshadow::Tensor<cpu,2> &temp = this->temp;
            Assert( batch.inst_index != NULL, "CXXAvgNetTrainer need inst_index" );
            for( index_t i = 0; i < temp.shape[1]; ++i ){
                unsigned ridx = batch.inst_index[ i ];
                Assert( ridx < num_avg_record, "inst_index exceed num_avg_record" );
                if( ref_counter[ ridx ] > this->round ) continue;
                ref_counter[ ridx ] = this->round + 1;
                int diff = this->round - num_burn;
                if( diff < 1 ) diff = 1;
                float alpha = 1.0f / diff;
                avg_pred[ridx] = (1.0f-alpha) * avg_pred[ridx] + alpha*temp[i];
                mshadow::Copy( temp[ i ], avg_pred[ridx] );
            }
        }
    private:
        inline void InitAvgRecord(void){
            ref_counter.resize( num_avg_record, 0 );
            mshadow::Shape<2> s = this->net.out().data[0][0].shape;
            avg_pred.Resize( mshadow::Shape2( num_avg_record, s[0] ), 0.0f );
            // mark avg record is available
            this->net.meta.param.reserved_flag = 1;
        }
    private:
        /*! \brief  number of burn in rounds, start averagin after this */
        int num_burn;
        /*! \brief  number of records to do averaging */
        unsigned num_avg_record;
        /*! \brief  reference counter */
        std::vector<int> ref_counter;
        /*! \brief  average prediction */
        mshadow::TensorContainer<cpu,2> avg_pred;
    };
}; // namespace cxxnet

namespace cxxnet{    
    template<typename xpu>
    class CXXNetSparseTrainer: public CXXNetTrainer<xpu>{
    public:
        CXXNetSparseTrainer(void){
            tparam.tag = "sparse";            
        }
        virtual ~CXXNetSparseTrainer(void){
        }
        virtual void SetParam( const char *name, const char *val ){
            if( !strcmp( name, "sparse:nhidden") ){
                char tmp[256];
                sparam.sparse_num_hidden = atoi(val);
                sprintf(tmp, "1,1,%d", sparam.sparse_num_hidden);
                CXXNetTrainer<xpu>::SetParam("input_shape", tmp);
                return;
            }
            if( !strcmp( name, "input_shape") ){
                unsigned x, y, z;
                Assert( sscanf( val, "%u,%u,%u", &z,&y,&x ) ==3,
                               "input_shape must be three consecutive integers without space example: 1,1,200 " );
                sparam.shape_in[0] = x; sparam.shape_in[1] = y; sparam.shape_in[2] = z;                
                utils::Assert( y == 1 && z == 1, "sparse net only accept vector as input" );
                return;
            }
            if( !strcmp( name, "sparse:init_sigma") ) sparam.sparse_init_sigma = (float)atof(val);
            if( !strcmp( name, "nthread") ){
                sparam.nthread = atoi(val);
                omp_set_num_threads(sparam.nthread);
            }
            tparam.SetParam(name, val);
            CXXNetTrainer<xpu>::SetParam(name, val);
        }
        virtual void InitModel( void ){
            CXXNetTrainer<xpu>::InitModel();
            Wsp.Resize( mshadow::Shape2( sparam.shape_in[0], sparam.sparse_num_hidden ) );            
            Wsp = this->net.rnd_cpu.gaussian( Wsp.shape ) * sparam.sparse_init_sigma;
            
            if( this->net.silent == 0 ){
                printf("CXXNetSparseTrainer: init with %ux%d connections\n", Wsp.shape[1], Wsp.shape[0]);
                printf("SparseSGDUpdater: eta=%f, init_sigma=%f\n", tparam.base_lr_, sparam.sparse_init_sigma );
            }
        }
        virtual void SaveModel( mshadow::utils::IStream &fo ) const {
            CXXNetTrainer<xpu>::SaveModel( fo );
            fo.Write( &sparam, sizeof(SparseNetParam) );
            Wsp.SaveBinary( fo );
        }
        virtual void LoadModel( mshadow::utils::IStream &fi ) {
            CXXNetTrainer<xpu>::LoadModel( fi );
            utils::Assert( fi.Read( &sparam, sizeof(SparseNetParam) ) != 0, "SparseNet" );
            Wsp.LoadBinary( fi );
        }
        virtual void Update ( const DataBatch& batch ) {
            CXXNetTrainer<xpu>::ForwardBackprop( batch, true );
            this->SparseUpdate( batch );
        }
    protected:
        virtual void MakeInput( const DataBatch& batch ){
            utils::Assert( batch.is_sparse(), "SparseNet: only accept sparse input");
            this->SparseForward( batch );
        }
    private:
        inline void SparseForward( const DataBatch& batch ){
            utils::Assert( this->net.in().is_mat(), "input must be vector" );
            mshadow::Tensor<xpu, 2> din = this->net.in().mat();
            node_hidden.Resize( din.shape ); 

            // maybe put a OpenMP here           
            #pragma omp parallel for schedule(static) 
            for( index_t i = 0; i < batch.batch_size; ++ i ){
                SparseInst line = batch.GetRowSparse(i);
                mshadow::Tensor<cpu,1> node = node_hidden[i];
                node = 0.0f;
                for( unsigned j = 0; j < line.length; ++ j ){
                    const SparseInst::Entry &e = line[j];
                    node += e.fvalue * Wsp[ e.findex ];
                }
            }
            mshadow::Copy( din, node_hidden );
        }

        inline void SparseUpdate( const DataBatch& batch ){
            utils::Assert( this->net.in().is_mat(), "input must be vector" );
            mshadow::Tensor<xpu, 2> din = this->net.in().mat();
            node_hidden.Resize( din.shape );
            mshadow::Copy( node_hidden, din );
            tparam.ScheduleEpoch( this->net.meta.param.num_epoch_passed );
                        
            // maybe put a OpenMP here            
            #pragma omp parallel for schedule(static)
            for( index_t i = 0; i < batch.batch_size; ++ i ){
                SparseInst line = batch.GetRowSparse(i);
                mshadow::Tensor<cpu,1> grad = node_hidden[i];
                for( unsigned j = 0; j < line.length; ++ j ){
                    const SparseInst::Entry &e = line[j];
                    Wsp[ e.findex ] += e.fvalue * (-tparam.learning_rate) * grad;
                    Wsp[ e.findex ] *= ( 1.0f - tparam.learning_rate * tparam.wd );
                }
            }
        }
    private:
        /*! \brief additional input for sparse net */
        struct SparseNetParam{
            /*! \brief intialize gaussian std for sparse layer */
            float sparse_init_sigma;
            /*! \brief number of hidden nodes in the first sparse layer */
            int sparse_num_hidden;
            /*! \brief input shape, not including batch dimension */
            mshadow::Shape<3> shape_in;
            /*! \brief number of threads used */
            int nthread;
            /*! \brief reserved field */
            int reserved[32];
            /*! \brief default constructor */
            SparseNetParam(void){
                nthread = 0;
                shape_in = mshadow::Shape3( 1, 1, 0 );
                sparse_init_sigma = 0.005f;
                sparse_num_hidden = 100;
                memset( reserved, 0, sizeof(reserved) );
            }            
        };
    private:
        /*! \brief training parameters */
        UpdaterParam tparam;
        /*! \brief neural net parameter */
        SparseNetParam sparam;
        /*! \brief temp hidden node for the model */
        mshadow::TensorContainer<cpu,2> node_hidden;
        mshadow::TensorContainer<cpu,2> Wsp;
    };
};

namespace cxxnet{
    template<typename xpu>
    INetTrainer* CreateNet_( int net_type ){
        switch( net_type ){
        case 0: return new CXXNetTrainer<xpu>();
        case 1: return new CXXAvgNetTrainer<xpu>();
        case 2: return new CXXNetSparseTrainer<xpu>();
        default: Error("unknown net type");
        }
        return NULL;
    }
}; // namespace cxxnet
#endif // CXXNET_NET_INL_HPP
