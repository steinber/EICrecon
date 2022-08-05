// Copyright 2022, David Lawrence
// Subject to the terms in the LICENSE file found in the top-level directory.
//
//


#include <sstream>

#include "EICRootWriter.h"
#include <JANA/JLogger.h>
#include <podio/GenericParameters.h>
#include <TInterpreter.h>

// datamodel_glue.h is generated automatically by make_datamodel_glue.py
#include "datamodel_glue.h"


//------------------------------------------------------------------------------
// DeriveCollectionName
//
/// Derive a name for the podio collection based on the edm4hep data type
/// and the factory data type and tag.
///
/// This is a bit of guesswork. It must handle 2 different scenarios:
///
/// 1. Objects in the factory came straight from the podio input file which
/// means they have a type from edm4hep and a factory tag with the collection
/// name.
///
/// 2. Objects are of specialized classes that inherit from an edm4hep type
/// and their collection name should be the derived type's class name.
///
/// For option 2. the factory tag could represent either an alternative
/// algorithm or a special category. For example, a factory producing
/// BEMCRawCalorimeterHit objects may have a factory tag like
/// "DaveTest" to indicate it is an alternative version of the algorithm
/// that the user may select at run time. In this case, we would want the
/// objects to be placed in the store in the standard place without any
/// reference to "DaveTest" in the name. Alternatively, the factory may
/// have a tag like "inner" indicating it holds a subset of the objects
/// so the tag really represents a category of the data. In this case we
/// may want to write to a collection name that includes this tag.
///
/// For now, we simply look to see if the data type for the factory
/// is the same as that for the edm4hep type. If they are the same, then
/// use the factory tag as the collection name. If not, use the factory's
/// data type as the name. If the tag is empty, always use the factory's
/// data type as the name.
///
/// \param edm4hep_name
/// \param fac
/// \return
//------------------------------------------------------------------------------
template< typename T>
std::string DeriveCollectionName( const std::string &edm4hep_name,  JFactory *fac ){

    // TODO: Should the factory store the collection name as a special metadata field?
    if( fac->GetTag() == "" ) return fac->GetObjectName();
    if( std::type_index( typeid(T) ) == fac->GetObjectType()){
        return fac->GetTag();
    }else{
        return fac->GetObjectName();
    }
}

//------------------------------------------------------------------------------
// PutPODIODataT
//
/// This templated global routine is used to copy objects from a JANA factory
/// into the EICEventStore so they can later be written to the output file.
///
/// This gets called from the PutPODIOData routine defined in the datamodel_glue.h
/// file which is generated by the make_datamodel_glue.py script.
///
/// The "OutputType" class is the edm4hep object type, while the "C" class is the collection type
/// that holds it. (e.g. OutputType=SimTrackerHit, C=SimTrackerHitCollection )
///
/// The return value is the collection name created by DeriveCollectionName() above.
///
/// \tparam T      data class type (e.g. edm4hep::Cluster)
/// \tparam C      collection type (e.g. edm4hep::ClusterCollection)
/// \param writer  EDM4hepWriter object (used only for include/exclude collection names)
/// \param fac     JANA factory holding objects of type OutputType
/// \param store   EICEventStore to copy the collection to
/// \return        derived name of the collection
//------------------------------------------------------------------------------
template <class T, class C>
std::string PutPODIODataT( EICRootWriter *writer, JFactory *fac,  EICEventStore &store){

    // Formulate appropriate collection name based on edm4hep data type name and factory data type and tag.
    C tmp;  // The getValueTypeName() method should be made static in the collection class.
    const std::string &className = tmp.getValueTypeName();
    std::string collection_name = DeriveCollectionName<T>( className,  fac );

    // Bail early if this is a collection the user indicated should be excluded.
    auto exclude_collections = writer->GetExcludeCollections();
    if( exclude_collections.count( collection_name ) ) return "";

    // Bail early if this is not a collection the user indicated should be included.
    auto include_collections = writer->GetExcludeCollections();
    if( (!include_collections.empty()) && (include_collections.count(collection_name)==0) ) return "";

    // Check if a collection with this name already exists. If not create it
    std::vector<T> *databuffer = nullptr;
    for( auto dv : store.m_datavectors ){
        if( dv->name == collection_name ){
            databuffer = static_cast<std::vector<T>*>( dv->GetVectorAddress() );
            break;
        }
    }
    if( databuffer == nullptr ) {
        auto dv = new EICEventStore::DataVectorT<T>(collection_name, className);
        store.m_datavectors.push_back( dv );
        databuffer = static_cast<std::vector<T>*>( dv->GetVectorAddress() );
    }

    // So this is pretty crazy. Podio provides no access at all to the underlying POD
    // data object through the high-level object. E.g. you cannot get a pointer to
    // the edm4hep::EventHeaderData object if all you have is the edm4hep::EventHeader
    // object. The only way to do this with the current API is to create a
    // edm4hep::EventHeaderCollection and fill it with clones of the edm4hep::EventHeader
    // objects we have. Then, ask the collection itself to prepare the write buffers
    // by copying the POD structures into a vector that we can then access. This
    // suffers an extra allocation of both the high-level and "Obj" level objects (with
    // the "Obj" level containing the POD-level data object).

    C collection;

    // Get data objects from JANA and copy into collection. Collection takes ownership.
    // Here, obj is a pointer to a high-level object (e.g. edm4hep::EventHeader)
    auto v = fac->GetAs<T>();
    for( auto obj : v ) collection->push_back( obj->clone() );  // <-- this is the efficiency killer

    // Tell the collection to push copies of the underlying POD data into contiguous
    // memory in the form of a std::vector<OutputType>.
    collection->prepareForWrite();
    auto mybuffers = collection.getBuffers();

    // Swap contents of the buffer created by our temporary collection with the one supplied by caller
    std::vector<T> *vecptr = mybuffers.template dataAsVector<T>();
    vecptr->swap(*databuffer);

    // At this point, all of the cloned objects are owned by the local collection and will be deleted
    // when the collection goes out of scope. The EventStore passed into us wil have copies of
    // the POD data.

    return collection_name;
}


//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
EICRootWriter::EICRootWriter() {
    SetTypeName(NAME_OF_THIS); // Provide JANA with this class's name
}

//------------------------------------------------------------------------------
// Init
//
/// Initialize by opening the output file and creating the TTree objects.
///
/// This is partially copied from here:
///
///   https://eicweb.phy.anl.gov/EIC/juggler/-/blob/master/JugBase/src/components/PodioOutput.cpp
///
//------------------------------------------------------------------------------
void EICRootWriter::Init() {
    // Get the output file name
    japp->SetDefaultParameter("PODIO:OUTPUT_FILE", m_OUTPUT_FILE, "Name of EDM4hep/podio output file to write to. Setting this will cause the output file to be created and written to.");

    // Allow user to set PODIO:OUTPUT_FILE to "1" to specify using the default name.
    if( m_OUTPUT_FILE == "1" ){
        auto param = japp->GetJParameterManager()->FindParameter("PODIO:OUTPUT_FILE" );
        if(param) {
            param->SetValue( param->GetDefault() );
            m_OUTPUT_FILE = param->GetDefault();
        }
    }

    // Get the output directory path for creating a second copy of the output file at the end of processing.
    // (this is duplicating similar functionality in Juggler/Gaudi so assume it is useful).
    japp->SetDefaultParameter("PODIO:OUTPUT_FILE_COPY_DIR", m_OUTPUT_FILE_COPY_DIR, "Directory name to make an additional copy of the output file to. Copy will be done at end of processing. Default is empty string which means do not make a copy. No check is made on path existing.");

    // Get the list of output collections to include
    // TODO: Convert this to using JANA support of array values in config parameters once it is available.
    japp->SetDefaultParameter("PODIO:OUTPUT_INCLUDE_COLLECTIONS", m_include_collections_str, "Comma separated list of collection names to write out. If not set, all collections will be written (including ones from input file). Don't set this and use PODIO:OUTPUT_EXCLUDE_COLLECTIONS to write everything except a selection.");
    if( ! m_include_collections_str.empty() ) {
        std::stringstream ss(m_include_collections_str);
        while (ss.good()) {
            std::string substr;
            getline(ss, substr, ',');
            m_OUTPUT_INCLUDE_COLLECTIONS.insert(substr);
        }
    }

    // Get the list of output collections to exclude
    // TODO: Convert this to using JANA support of array values in config parameters once it is available.
    japp->SetDefaultParameter("PODIO:OUTPUT_EXCLUDE_COLLECTIONS", m_exclude_collections_str, "Comma separated list of collection names to not write out.");
    if( ! m_exclude_collections_str.empty() ) {
        std::stringstream ss(m_exclude_collections_str);
        while (ss.good()) {
            std::string substr;
            getline(ss, substr, ',');
            m_OUTPUT_EXCLUDE_COLLECTIONS.insert(substr);
        }
    }

    // Open output file
    m_file = std::unique_ptr<TFile>(TFile::Open(m_OUTPUT_FILE.c_str(), "RECREATE", "data file"));

    // Create trees
    m_datatree     = new TTree("events",       "Events tree");
    m_metadatatree = new TTree("metadata",     "Metadata tree");
    m_runMDtree    = new TTree("run_metadata", "Run metadata tree");
    m_evtMDtree    = new TTree("evt_metadata", "Event metadata tree");
    m_colMDtree    = new TTree("col_metadata", "Collection metadata tree");

    m_evtMDtree->Branch("evtMD", "GenericParameters", &m_evtMD );
}

//------------------------------------------------------------------------------
// CreateBranch
///
/// Create the appropriate branches in the events TTree to hold the given collection.
///
/// \param dv EICEventStore::DataVector with collection corresponding to branch we want to create
//------------------------------------------------------------------------------
void EICRootWriter::CreateBranch(EICEventStore::DataVector *dv) {

    auto branch = m_datatree->Branch(dv->name.c_str(), dv->className.c_str(), static_cast<void*>(dv->GetVectorAddressPtr()));
    m_collection_branches[dv->name] = dv->className;

//    // Create branches for collections holding relations
//    if (auto* refColls = references) {
//        int j = 0;
//        for (auto& c : (*refColls)) {
//            const auto brName = refBranch(collName, j);
//            m_datatree->Branch(brName.c_str(), c.get());
//            ++j;
//        }
//    }
//    // vector members
//    if (auto* vminfo = vecmembers) {
//        int j = 0;
//        for (auto& [dataType, add] : (*vminfo)) {
//            const std::string typeName = "vector<" + dataType + ">";
//            const auto brName          = vecBranch(collName, j);
//            m_datatree->Branch(brName.c_str(), typeName.c_str(), add);
//            ++j;
//        }
//    }
//
//    const auto collID = m_store.getCollectionIDTable()->collectionID(collName);
//    const auto collType = collBase->getValueTypeName() + "Collection";
//    const auto collInfo = std::tuple<int, std::string, bool>(collID, collType, collBase->isSubsetCollection());
//    m_collectionInfo.push_back(collInfo);

    // Backfill for events we've missed.
    // This branch may not be created until some events have already been processed. In order to align
    // future events, we need to insert empty events for this branch. To do this, we need a data pointer
    // that points to an empty vector (data currently points to a vector, but one that is not empty).
    // Ideally we would make a std:vector of the correct type here, but that is a little difficult and
    // adds extra overhead. Instead, we make a temporary std::vector<uint64_t> with the assumption that
    // root will see it has zero elements and do the correct thing.
    auto tmpdv = MakeDataVector(dv->name, dv->className);
    branch->SetAddress( static_cast<void*>(tmpdv->GetVectorAddressPtr()) );
    auto Nentries = m_datatree->GetEntries();
    for( size_t i=0; i< Nentries; i++ ) branch->Fill();

    // This is not really needed since ResetBranches will do it anyway. Resetting it here though may
    // provide some future-proofing.
    branch->SetAddress( static_cast<void*>(dv->GetVectorAddressPtr()) );
}

//------------------------------------------------------------------------------
// ResetBranches
//
/// Reset TTree branch addresses for all specified collections. This ensures
/// all branches are pointing to the correct memory locations since they
/// may have changed since the last event.
///
/// Note: This will create a new branch in the events TTree if it does not
/// already exist for a the collection.
///
/// Note: User specified include/exclude lists are applied here.
///
/// \param store  collections to setup/create branch addresses for
//------------------------------------------------------------------------------
void EICRootWriter::ResetBranches(EICEventStore &store) {

    // store should now contain all data we need to write out. The
    // name of each data vector is the collection name and the className
    // is the class (e.g. edm4hep::EventStore).
    for (auto dv : store.m_datavectors) {

        // Ignore if this is a collection the user indicated should be excluded.
        if( m_OUTPUT_EXCLUDE_COLLECTIONS.count( dv->name ) ) continue;

        // Ignore if this is not a collection the user indicated should be included.
        if( (!m_OUTPUT_INCLUDE_COLLECTIONS.empty()) && (m_OUTPUT_INCLUDE_COLLECTIONS.count(dv->name)==0) ) continue;

        // Check if the branch already exists. If not, create it
        if( ! m_collection_branches.count(dv->name)  ) CreateBranch( dv );

        // Set the branch address to point to the existing std::vector of POD data
        // n.b. static_cast is so we don't match templated SetBranchAddress with void**. That
        // leads to warnings/errors with finding dictionaries.
        m_datatree->SetBranchAddress(dv->name.c_str(), static_cast<void*>(dv->GetVectorAddressPtr()));

        // Make sure this collection is in the list of collection ids that will be written in Finish()
        if( ! m_collectionIDtable.present(dv->name) ) m_collectionIDtable.add(dv->name);

//        auto buffers = collBuffers->getBuffers();
//        auto* data = buffers.data;
//        auto* references = buffers.references;
//        auto* vecmembers = buffers.vectorMembers;
//
//        // Reconnect branches and collections
//        m_datatree->SetBranchAddress(collName.c_str(), data);
//        auto* colls = references;
//        if (colls != nullptr) {
//            for (size_t j = 0; j < colls->size(); ++j) {
//                const auto brName = refBranch(collName, j);
//                auto* l_branch = m_datatree->GetBranch(brName.c_str());
//                l_branch->SetAddress(&(*colls)[j]);
//            }
//        }
//        auto* colls_v = vecmembers;
//        if (colls_v != nullptr) {
//            int j = 0;
//            for (auto& [dataType, add] : (*colls_v)) {
//                const auto brName = vecBranch(collName, j);
//                m_datatree->SetBranchAddress(brName.c_str(), add);
//                ++j;
//            }
//        }
    }
}

//------------------------------------------------------------------------------
// Process
//
/// Process single event, writing it to the TTrees in the root file.
/// This will write out all objects already in the factories, it
/// currently does not activate any factory algorithms to generate
/// objects. Thus, this should be at the end of any plugin list.
///
/// TODO: This should automatically activate factories corresponding
/// TODO: to the collections specified for writing out.
///
/// \param event
//------------------------------------------------------------------------------
void EICRootWriter::Process(const std::shared_ptr<const JEvent> &event) {

    // Place all values we plan to write into an EICEventStore object
    EICEventStore store;

    // If an EICEventStore already exists for this event, we should use it so
    // that we save time/memory copying duplicate objects into it. We actually
    // use it by temporarily swapping the contents of its member vectors with
    // those in our local "store".
    auto es = event->GetSingle<EICEventStore>();
    if( es ) store.Swap( const_cast<EICEventStore*>(es) );

    // Loop over all factories.
    for( auto fac : event->GetAllFactories() ){

        // Attempt to put data from all factories that have objects into the
        // store. This is called even for ones whose data classes don't inherit from
        // an edm4hep class. Those cases just silently do nothing here and return
        // an empty string. Note that this relies on the JFactory::EnableAs mechanism
        // so that needs to have been called in the factory constructor.
        if( fac->GetNumObjects() != 0 ){
            try {
                auto collection_name = PutPODIOData(this, fac, store);
            }catch(std::exception &e){
                std::cerr << e.what() << " : " << fac->GetObjectName() << std::endl;
            }

        }
     }

    // Lock mutex so we can modify ROOT trees
    // TODO: This needs to be changed to use the global ROOT write lock.
    std::lock_guard<std::mutex>lock(m_mutex);

    // Reset the branch addresses for all collections we are writing
    ResetBranches( store );

    // Write the event to trees
    if( m_datatree  ) m_datatree->Fill();
    if( m_evtMDtree ) m_evtMDtree->Fill();

    // Swap the EICEventStore members back to the JANA-managed one in case another processor
    // downstream wants to use it
    // TODO: We are violating a JANA design principle by potentially modifying the EICEventStore
    // TODO: object by adding collections to it.
    if( es ) store.Swap( const_cast<EICEventStore*>(es) );
}

//------------------------------------------------------------------------------
// Finish
//
/// Called once automatically by JANA at end of job. Flushes trees and closes
/// output files. This also creates/fills branches with additional metadata
/// gathered will processing the job. This must be called to have a valid
/// podio/edm4hep root file.
///
/// TODO: Add JANA configuration parameters as metadata to file.
//------------------------------------------------------------------------------
void EICRootWriter::Finish() {

    LOG << "Finalizing trees and output file" << LOG_END;
    m_file->cd();
    //m_metadatatree->Branch("gaudiConfigOptions", &config_data);
    // TODO: Copy all JANA configuration parameters into the metadata tree.
    // TODO: This will be most easily done when JANA issue #120 is resolved
    // TODO: so we can easily access the full list.

    // Fill in the CollectionTypeInfo table based on collectionID table,
    // For now, mark all as not being a "subset"
    for( auto name : m_collectionIDtable.names() ) {
        if( ! m_collection_branches.count(name) ) continue; // TODO: make this an error?
        const auto collID = m_collectionIDtable.collectionID(name);

        // className is the value of the m_collection_branches map but is of form
        // "vector<edm4hep::EventHeader>". Need to extract template parameter type.
        const auto &className = m_collection_branches[name];
        auto start_pos = className.find_first_of("<");
        auto end_pos   = className.find_last_of(">");
        if( start_pos==std::string::npos || end_pos==std::string::npos ) continue; // TODO: make this an error?
        auto collType = className.substr(start_pos+1, end_pos-start_pos-1) + "Collection";

        // Add to collectionInfo
        const auto collInfo = std::tuple<int, std::string, bool>(collID, collType, false); // TODO: support issubset!=false
        m_collectionInfo.push_back(collInfo);
    }

    podio::version::Version podioVersion = podio::version::build_version;
    m_metadatatree->Branch("PodioVersion", &podioVersion);
    m_metadatatree->Branch("CollectionTypeInfo", &m_collectionInfo);
    m_metadatatree->Branch("CollectionIDs", &m_collectionIDtable);
    m_metadatatree->Fill();
    m_colMDtree->Branch("colMD", "std::map<int,podio::GenericParameters>", &m_colMetaDataMap ) ;
    m_colMDtree->Fill();
    m_runMDtree->Branch("runMD", "std::map<int,podio::GenericParameters>", &m_runMetaDataMap ) ;
    m_runMDtree->Fill();
    m_datatree->Write();
    m_file->Write();
    m_file->Close();
    m_datatree = nullptr;
    m_evtMDtree = nullptr;
    LOG << "Data written to: " << m_OUTPUT_FILE << LOG_END;

    // Optionally copy file to a second location.
    // This is just duplicating what was done in the Juggler/Gaudi implementation.
    if (!m_OUTPUT_FILE_COPY_DIR.empty()) {
        TFile::Cp(m_OUTPUT_FILE.c_str(), m_OUTPUT_FILE_COPY_DIR.c_str(), false);
        LOG << " and copied to: " << m_OUTPUT_FILE_COPY_DIR << LOG_END;
    }

}



