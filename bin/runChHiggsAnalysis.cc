#include <iostream>
#include <boost/shared_ptr.hpp>
#include <fstream>

#include "UserCode/llvv_fwk/interface/MacroUtils.h"
#include "UserCode/llvv_fwk/interface/SmartSelectionMonitor.h"
#include "UserCode/llvv_fwk/interface/DataEventSummaryHandler.h"
#include "UserCode/llvv_fwk/interface/LxyAnalysis.h"
#include "UserCode/llvv_fwk/interface/UEAnalysis.h"
#include "UserCode/llvv_fwk/interface/BTVAnalysis.h"
#include "UserCode/llvv_fwk/interface/LeptonEfficiencySF.h"
#include "UserCode/llvv_fwk/interface/MuScleFitCorrector.h"

#include "CondFormats/JetMETObjects/interface/JetResolution.h"
#include "CondFormats/JetMETObjects/interface/JetCorrectionUncertainty.h"
#include "CondFormats/JetMETObjects/interface/JetCorrectorParameters.h"

#include "FWCore/FWLite/interface/AutoLibraryLoader.h"
#include "FWCore/PythonParameterSet/interface/MakeParameterSets.h"
#include "FWCore/ParameterSet/interface/ParameterSet.h"

#include "PhysicsTools/Utilities/interface/LumiReWeighting.h"

#include "PhysicsTools/CondLiteIO/interface/RecordWriter.h"
#include "DataFormats/FWLite/interface/Record.h"
#include "DataFormats/FWLite/interface/EventSetup.h"
#include "DataFormats/FWLite/interface/ESHandle.h"
#include "CondFormats/PhysicsToolsObjects/interface/BinningPointByMap.h"
#include "RecoBTag/PerformanceDB/interface/BtagPerformance.h"
#include "PhysicsTools/FWLite/interface/CommandLineParser.h"

#include "TSystem.h"
#include "TFile.h"
#include "TChain.h"
#include "TChainElement.h"
#include "TCanvas.h"
#include "TString.h"
#include "TDirectory.h"
#include "TEventList.h"
#include "TRandom.h"

#include <iostream>

using namespace std;



int main(int argc, char* argv[])
{
  // load framework libraries
  gSystem->Load( "libFWCoreFWLite" );
  AutoLibraryLoader::enable();

  //check arguments
  if ( argc < 2 ) {
    std::cout << "Usage : " << argv[0] << " parameters_cfg.py" << std::endl;
    return 0;
  }
  
  //
  // configure
  //
  const edm::ParameterSet &runProcess = edm::readPSetsFrom(argv[1])->getParameter<edm::ParameterSet>("runProcess");
  std::vector<std::string> urls=runProcess.getParameter<std::vector<std::string> >("input");
  TString url = TString(urls[0]);
  TString baseDir    = runProcess.getParameter<std::string>("dirName");
  bool runSystematics = runProcess.getParameter<bool>("runSystematics");
  TString jecDir      = runProcess.getParameter<std::string>("jecDir");
  bool isMC          = runProcess.getParameter<bool>("isMC");
  int mcTruthMode    = runProcess.getParameter<int>("mctruthmode");
  double xsec        = runProcess.getParameter<double>("xsec");
  bool isV0JetsMC(isMC && (url.Contains("DYJetsToLL_50toInf") || url.Contains("WJets")));
  TString out        = runProcess.getParameter<std::string>("outdir");
  bool saveSummaryTree = runProcess.getParameter<bool>("saveSummaryTree");

  //jet energy scale uncertainties
  gSystem->ExpandPathName(jecDir);
  FactorizedJetCorrector *jesCor        = utils::cmssw::getJetCorrector(jecDir,isMC);
  JetCorrectionUncertainty *totalJESUnc = new JetCorrectionUncertainty((jecDir+"/MC_Uncertainty_AK5PFchs.txt").Data());

  //muon energy scale and uncertainties
  MuScleFitCorrector *muCor=getMuonCorrector(jecDir,url);

  // FIXME: add dy reweighting

 
  //
  // check input file
  //
  TFile *inF = TFile::Open(url);
  if(inF==0) return -1;
  if(inF->IsZombie()) return -1;
  TString proctag=gSystem->BaseName(url);
  Ssiz_t pos=proctag.Index(".root");
  proctag.Remove(pos,proctag.Length());

  bool filterOnlyEE(false), filterOnlyEMU(false), filterOnlyMUMU(false);
  if(!isMC)
    {
      if(url.Contains("DoubleEle")) filterOnlyEE=true;
      if(url.Contains("DoubleMu"))  filterOnlyMUMU=true;
      if(url.Contains("MuEG"))      filterOnlyEMU=true;
    }
  
  //
  // pileup reweighter
  //
  std::vector<double> dataPileupDistributionDouble = runProcess.getParameter< std::vector<double> >("datapileup");
  std::vector<float> dataPileupDistribution; for(unsigned int i=0;i<dataPileupDistributionDouble.size();i++){dataPileupDistribution.push_back(dataPileupDistributionDouble[i]);}
  std::vector<float> mcPileupDistribution;
  if(isMC){
    TString puDist(baseDir+"/pileup");
    TH1F* histo = (TH1F *) inF->Get(puDist);
    if(!histo)std::cout<<"pileup histogram is null!!!\n";
    for(int i=1;i<=histo->GetNbinsX();i++){mcPileupDistribution.push_back(histo->GetBinContent(i));}
    delete histo;
  }
  while(mcPileupDistribution.size()<dataPileupDistribution.size())  mcPileupDistribution.push_back(0.0);
  while(mcPileupDistribution.size()>dataPileupDistribution.size())dataPileupDistribution.push_back(0.0);
  gROOT->cd();  //THIS LINE IS NEEDED TO MAKE SURE THAT HISTOGRAM INTERNALLY PRODUCED IN LumiReWeighting ARE NOT DESTROYED WHEN CLOSING THE FILE
  edm::LumiReWeighting *LumiWeights= isMC ? new edm::LumiReWeighting(mcPileupDistribution,dataPileupDistribution): 0;
  utils::cmssw::PuShifter_t PuShifters;
  if(isMC) PuShifters=utils::cmssw::getPUshifters(dataPileupDistribution,0.05);

  //systematics
  std::vector<TString> systVars(1,"");
  if(runSystematics && isMC)
    {
      systVars.push_back("_jerup" ); systVars.push_back("_jerdown"   );
      systVars.push_back("_jesup" ); systVars.push_back("_jesdown"   );
      //systVars.push_back("_lesup" ); systVars.push_back("_lesdown"   );
      systVars.push_back("_leffup"); systVars.push_back("_leffdown"  );
      systVars.push_back("_puup"  ); systVars.push_back("_pudown"    );
      systVars.push_back("_umetup"); systVars.push_back( "_umetdown" );
      cout << "Systematics will be computed for this analysis - this will take a bit" << endl;
    }
  
  //
  // control histograms
  //
  SmartSelectionMonitor controlHistos;
  TH1F* Hhepup        = (TH1F* )controlHistos.addHistogram(new TH1F ("heupnup"    , "hepupnup"    ,20,0,20) ) ;
  TH1F* Hcutflow      = (TH1F*) controlHistos.addHistogram(new TH1F ("cutflow"    , "cutflow"    ,5,0,5) ) ;
  TH1F* Hoptim_systs = (TH1F*) controlHistos.addHistogram(new TH1F ("optim_systs" , ";syst;", systVars.size(),0,systVars.size()) );



  //vertex multiplicity
  controlHistos.addHistogram( new TH1F ("nvertices", "; Vertex multiplicity; Events", 50, 0.,50.) );

  TString labels[]={"2 leptons", "M>12 #wedge |M-M_{Z}|>15", "#geq 2 jets", "E_{T}^{miss}", "op. sign", "#geq 2 b-tags"};
  int nsteps=sizeof(labels)/sizeof(TString);


  //event selection histogram - per systematic variation
  for(size_t ivar=0;ivar<systVars.size();++ivar){
    TString var=systVars[ivar];

    Hoptim_systs->GetXaxis()->SetBinLabel(ivar+1, var);
    
    TH1F *cutflowH = (TH1F *)controlHistos.addHistogram( new TH1F("evtflow"+var,";Cutflow;Events",nsteps,0,nsteps) );
    for(int ibin=0; ibin<nsteps; ibin++) cutflowH->GetXaxis()->SetBinLabel(ibin+1,labels[ibin]);
    
    //    TString ctrlCats[]={"","eq1jets","lowmet","eq1jetslowmet","zlowmet","zeq1jets","zeq1jetslowmet","z"};
    TString ctrlCats[]={""};
    for(size_t k=0;k<sizeof(ctrlCats)/sizeof(TString); k++)
      {
	controlHistos.addHistogram( new TH1F(ctrlCats[k]+"emva"+var, "; e-id MVA; Electrons", 50, 0.95,1.0) );
	controlHistos.addHistogram( new TH1F(ctrlCats[k]+"mll"+var,";Dilepton invariant mass [GeV];Events",50,0,250) );
	controlHistos.addHistogram( new TH1F(ctrlCats[k]+"ptll"+var,";Dilepton transverse momentum [GeV];Events",50,0,250) );
	controlHistos.addHistogram( new TH1F(ctrlCats[k]+"pte"+var,";Electron transverse momentum [GeV];Events",50,0,500) );
	controlHistos.addHistogram( new TH1F(ctrlCats[k]+"ptmu"+var,";Muon transverse momentum [GeV];Events",50,0,500) );
	controlHistos.addHistogram( new TH1F(ctrlCats[k]+"ptmin"+var,";Minimum lepton transverse momentum [GeV];Events",50,0,500) );
	controlHistos.addHistogram( new TH1F(ctrlCats[k]+"dilarccosine"+var,";#theta(l,l') [rad];Events",50,0,3.2) );
	controlHistos.addHistogram( new TH1F(ctrlCats[k]+"mtsum"+var,";M_{T}(l^{1},E_{T}^{miss})+M_{T}(l^{2},E_{T}^{miss}) [GeV];Events",100,0,1000) );
	controlHistos.addHistogram( new TH1F(ctrlCats[k]+"met"+var,";Missing transverse energy [GeV];Events",50,0,500) );
	
	controlHistos.addHistogram( new TH1F(ctrlCats[k]+"ht"+var,";H_{T} [GeV];Events",50,0,1000) );
	controlHistos.addHistogram( new TH1F(ctrlCats[k]+"htb"+var,";H_{T} (bjets) [GeV];Events",50,0,1000) );
	controlHistos.addHistogram( new TH1F(ctrlCats[k]+"htnol"+var,"; H_[T] (no leptons) [GeV];Events",50,0,1000) );
	controlHistos.addHistogram( new TH1F(ctrlCats[k]+"htbnol"+var,"; H_[T] (bjets, no leptons) [GeV];Events",50,0,1000) );
	
	TH1F *h=(TH1F *)controlHistos.addHistogram( new TH1F(ctrlCats[k]+"njets"+var,";Jet multiplicity;Events",8,0,8) );
	TH1F *hb=(TH1F *)controlHistos.addHistogram( new TH1F(ctrlCats[k]+"nbjets"+var,";b-Jet multiplicity;Events",8,0,8) );
	for(int ibin=1; ibin<=h->GetXaxis()->GetNbins(); ibin++)
	  {
	    TString label( ibin==h->GetXaxis()->GetNbins() ? "#geq" : "=");
	    label += (ibin-1);
	    label += " jets";
	    h->GetXaxis()->SetBinLabel(ibin,label);
	    hb->GetXaxis()->SetBinLabel(ibin,label);
	    if(ibin==1) continue;
	    label="jet"; label+=(ibin-1);
	    controlHistos.addHistogram( new TH1F(ctrlCats[k]+"pt"+label+"pt"+var,";Transverse momentum [GeV];Events",50,0,250) );
	    controlHistos.addHistogram( new TH1F(ctrlCats[k]+"pt"+label+"nobsmearpt"+var,";Transverse momentum [GeV];Events",50,0,250) );
	    controlHistos.addHistogram( new TH1F(ctrlCats[k]+"pt"+label+"smearpt"+var,";Transverse momentum [GeV];Events",50,0,250) );
	    controlHistos.addHistogram( new TH1F(ctrlCats[k]+"pt"+label+"eta"+var,";Pseudo-rapidity;Events",50,0,2.5) );
	    TH1F *flav1H=(TH1F *)controlHistos.addHistogram( new TH1F(ctrlCats[k]+"pt"+label+"flav"+var,";Flavor;Events",5,0,5) );
	    controlHistos.addHistogram( new TH1F(ctrlCats[k]+"btag"+label+"pt"+var,";Transverse momentum [GeV];Events",50,0,250) );
	    controlHistos.addHistogram( new TH1F(ctrlCats[k]+"btag"+label+"eta"+var,";Pseudo-rapidity;Events",50,0,2.5) );
	    TH1F *flav2H=(TH1F *)controlHistos.addHistogram( new TH1F(ctrlCats[k]+"btag"+label+"flav"+var,";Flavor;Events",5,0,5) );
	    for(int ibin=1; ibin<=5; ibin++)
	      {
		TString label("unmatched");
		if(ibin==2) label="g";
		if(ibin==3) label="uds";
		if(ibin==4) label="c";
		if(ibin==5) label="b";
		flav1H->GetXaxis()->SetBinLabel(ibin,label);
		flav2H->GetXaxis()->SetBinLabel(ibin,label);
	      }
	  }
      }
  }
  //lepton efficiencies
  LeptonEfficiencySF lepEff;

  //UEAnalysis ueAn(controlHistos); // FIXME: implement runSystematics here or add a different class for managing histos of interest :)
  //  BTVAnalysis btvAn(controlHistos,runSystematics);
  //  LxyAnalysis lxyAn(controlHistos,runSystematics);
  
  ///
  // process events file
  //
  DataEventSummaryHandler evSummary;
  if( !evSummary.attach( (TTree *) inF->Get(baseDir+"/data") ) )  { inF->Close();  return -1; }  
  const Int_t totalEntries=evSummary.getEntries();
  
  float cnorm=1.0;
  if(isMC){
    TH1F* cutflowH = (TH1F *) inF->Get(baseDir+"/cutflow");
    if(cutflowH) cnorm=cutflowH->GetBinContent(1);
  }
  Hcutflow->SetBinContent(1,cnorm);

  cout << "Processing: " << proctag << " @ " << url << endl
       << "Initial number of events: " << cnorm << endl
       << "Events in tree:           " << totalEntries << endl
       << " xSec x BR:               " << xsec << endl;


  //check if a summary should be saved
  Float_t evSummaryWeight(1.0);
  Float_t xsecWeight(isMC ? xsec/cnorm : 1.0);
  TFile *spyFile=0;
  TDirectory *spyDir=0;
  DataEventSummaryHandler *spyEvents=0;
  if(saveSummaryTree)
    {
      gSystem->Exec("mkdir -p " + out);
      gDirectory->SaveSelf();
      TString summaryName(out + "/" + proctag);
      if(mcTruthMode!=0) { summaryName += "_filt"; summaryName += mcTruthMode; } 
      summaryName += "_summary.root";
      gSystem->ExpandPathName(summaryName);
      cout << "Creating event summary file @ " << summaryName << endl;

      //open file
      spyEvents = new DataEventSummaryHandler;
      spyFile = TFile::Open(summaryName,"RECREATE");
      spyFile->rmdir(proctag);
      spyDir = spyFile->mkdir(proctag);
      TTree *outT = evSummary.getTree()->CloneTree(0);
      outT->SetTitle("Event summary");
      outT->SetDirectory(spyDir);
      outT->SetAutoSave(1000000);
      outT->Branch("weight",&evSummaryWeight,"weight/F"); 
      spyEvents->init(outT,false);
    }

       
  //
  // analyze (puf...)
  //
  DuplicatesChecker duplicatesChecker;
  int nDuplicates(0);
  for (int inum=0; inum < totalEntries; ++inum)
    {
      if(inum%500==0) { printf("\r [ %d/100 ]",int(100*float(inum)/float(totalEntries))); cout << flush; }
      evSummary.getEntry(inum);
      DataEventSummary &ev = evSummary.getEvent();
      if(!isMC && duplicatesChecker.isDuplicate( ev.run, ev.lumi, ev.event) ) { nDuplicates++; continue; }
      for(size_t ivar=0; ivar<systVars.size(); ++ivar){
	TString var=systVars[ivar];

	//pileup weight
	float weightNom(1.0),weightUp(1.0), weightDown(1.0);
	if(LumiWeights) {
	  weightNom     = LumiWeights->weight(ev.ngenITpu);
	  weightUp   = weightNom*PuShifters[utils::cmssw::PUUP]->Eval(ev.ngenITpu);
	  weightDown = weightNom*PuShifters[utils::cmssw::PUDOWN]->Eval(ev.ngenITpu);
	}

	if(isV0JetsMC && ev.nup>5)                          continue;
	Hhepup->Fill(ev.nup,weightNom);

	//MC truth (filtering for other ttbar)
	data::PhysicsObjectCollection_t gen=evSummary.getPhysicsObject(DataEventSummaryHandler::GENPARTICLES);
	bool hasTop(false);
	int ngenLeptonsStatus3(0);
	if(isMC)
	  {
	    for(size_t igen=0; igen<gen.size(); igen++){
	      if(gen[igen].get("status")!=3) continue;
	      int absid=abs(gen[igen].get("id"));
	      if(absid==6) hasTop=true;
	      if(absid!=11 && absid!=13 && absid!=15) continue;
	      ngenLeptonsStatus3++;
	    }
	    if(mcTruthMode==1 && (ngenLeptonsStatus3!=2 || !hasTop)) continue;
	    if(mcTruthMode==2 && (ngenLeptonsStatus3==2 || !hasTop)) continue;
	  }

	Hcutflow->Fill(1,1);
	Hcutflow->Fill(2,weightNom);
	Hcutflow->Fill(3,weightUp);
	Hcutflow->Fill(4,weightDown);
	
	

	//trigger bits
	bool eeTrigger   = ev.t_bits[0];
	bool emuTrigger  = ev.t_bits[4] || ev.t_bits[5];
	bool mumuTrigger = ev.t_bits[2] || ev.t_bits[3];
	if(filterOnlyEE)   {                   emuTrigger=false;  mumuTrigger=false; }
	if(filterOnlyEMU)  { eeTrigger=false;                     mumuTrigger=false; }
	if(filterOnlyMUMU) { eeTrigger=false;  emuTrigger=false;                     }
	
	data::PhysicsObjectCollection_t leptons=evSummary.getPhysicsObject(DataEventSummaryHandler::LEPTONS);
	data::PhysicsObjectCollection_t selLeptons;
	for(size_t ilep=0; ilep<leptons.size(); ilep++)
	  {
	    Int_t id=leptons[ilep].get("id");
	    bool passKin(true),passId(true),passIso(true);
	    if(abs(id)==11)
	      {
		float sceta=leptons[ilep].getVal("sceta");
		Float_t gIso    = leptons[ilep].getVal("gIso03");
		Float_t chIso   = leptons[ilep].getVal("chIso03");
		//Float_t puchIso = leptons[ilep].getVal("puchIso03");
		Float_t nhIso   = leptons[ilep].getVal("nhIso03");
		float relIso=(TMath::Max(nhIso+gIso-ev.rho*utils::cmssw::getEffectiveArea(11,sceta),Float_t(0.))+chIso)/leptons[ilep].pt();
		if(leptons[ilep].pt()<20)                      passKin=false;
		if(fabs(leptons[ilep].eta())>2.5)              passKin=false;
		if(fabs(sceta)>1.4442 && fabs(sceta)<1.5660)   passKin=false;
		if(leptons[ilep].getFlag("isconv"))            passId=false;
		if(leptons[ilep].getVal("tk_d0")>0.4)          passId=false;
		if(leptons[ilep].getVal("tk_lostInnerHits")>0) passId=false;
		if(leptons[ilep].getVal("mvatrig")<0.5)        passId=false;
		if(relIso>0.15)                                passIso=false;
	      }
	    else
	      {

		if(muCor){
		  TLorentzVector p4(leptons[ilep].px(),leptons[ilep].py(),leptons[ilep].pz(),leptons[ilep].energy());
		  muCor->applyPtCorrection(p4 , id<0 ? -1 : 1 );
		  if(isMC) muCor->applyPtSmearing(p4, id<0 ? -1 : 1, false);
		  leptons[ilep].SetPxPyPzE(p4.Px(),p4.Py(),p4.Pz(),p4.E());
		}
		
		Int_t idbits    = leptons[ilep].get("idbits");
		bool isTight    = ((idbits >> 10) & 0x1);
		//bool isLoose    = ((idbits >> 8) & 0x1);
		Float_t gIso    = leptons[ilep].getVal("gIso04");
		Float_t chIso   = leptons[ilep].getVal("chIso04");
		Float_t puchIso = leptons[ilep].getVal("puchIso04");
		Float_t nhIso   = leptons[ilep].getVal("nhIso04");
		Float_t relIso=(TMath::Max(nhIso+gIso-0.5*puchIso,0.)+chIso)/leptons[ilep].pt();
		if(leptons[ilep].pt()<20)                      passKin=false;
		if(fabs(leptons[ilep].eta())>2.4)              passKin=false;
		if(!isTight)                                   passId=false;
		//if(!isLoose)                                   passId=false;
		if(relIso>0.12)                                passIso=false;
	      }
	    
	    if(!passKin || !passId || !passIso) continue;
	    selLeptons.push_back(leptons[ilep]);
	  }
	sort(selLeptons.begin(),selLeptons.end(),data::PhysicsObject_t::sortByPt);
	
	//select the leptons
	if(!eeTrigger && !emuTrigger && !mumuTrigger) continue;
	if(selLeptons.size()<2) continue;
	
	//apply data/mc correction factors
	ev.cat=1;
	//	int dilId(1);
	float llScaleFactor(1.0), llScaleFactor_plus(1.0), llScaleFactor_minus(1.0);
	for(size_t ilep=0; ilep<2; ilep++)
	  {
	    //dilId *= selLeptons[ilep].get("id");
	    ev.cat *= selLeptons[ilep].get("id");
	    int id(abs(selLeptons[ilep].get("id")));
	    std::pair<float,float> ieff= lepEff.getLeptonEfficiency( selLeptons[ilep].pt(), selLeptons[ilep].eta(), id,  id ==11 ? "loose" : "tight" );
	    llScaleFactor *= isMC ? ieff.first : 1.0;
	    llScaleFactor_plus  *= (1.0+ieff.second );
	    llScaleFactor_minus *= (1.0-ieff.second );
	  }

	float weight(weightNom);	
	// PU shift
	if(var=="puup")   weight = weightUp;
	if(var=="pudown") weight = weightDown;
	
	weight *= llScaleFactor;

	if(var=="leffup")   weight *= llScaleFactor_plus;
	if(var=="leffdown") weight *= llScaleFactor_minus;
	
	//set the channel
	TString chName;
	//	bool isOS(dilId<0);
	bool isOS(ev.cat<0);
	bool isSameFlavor(false);
	if     (abs(ev.cat/*dilId*/)==11*11 && eeTrigger)   { chName="ee";  isSameFlavor=true;  if(ngenLeptonsStatus3>=2) llScaleFactor*=0.972; }
	else if(abs(ev.cat/*dilId*/)==11*13 && emuTrigger)  { chName="emu";                     if(ngenLeptonsStatus3>=2) llScaleFactor*=0.968; }
	else if(abs(ev.cat/*dilId*/)==13*13 && mumuTrigger) { chName="mumu"; isSameFlavor=true; if(ngenLeptonsStatus3>=2) llScaleFactor*=0.955; }
	else                                       continue;
	std::vector<TString> ch(1,chName);
	if(isSameFlavor) ch.push_back("ll");
	

	//the met
	data::PhysicsObjectCollection_t recoMet=evSummary.getPhysicsObject(DataEventSummaryHandler::MET);
	//select the jets
	data::PhysicsObjectCollection_t jets=evSummary.getPhysicsObject(DataEventSummaryHandler::JETS);
	utils::cmssw::updateJEC(jets,jesCor,totalJESUnc,ev.rho,ev.nvtx,isMC);
	std::vector<LorentzVector> metVars=utils::cmssw::getMETvariations(recoMet[0],jets,selLeptons,isMC);
	
	int metIdx(0);
	if(var == "jerup")    metIdx=utils::cmssw::JERUP;   
	if(var == "jerdown")  metIdx=utils::cmssw::JERDOWN; 
	if(var == "jesup")    metIdx=utils::cmssw::JESUP;   
	if(var == "jesdown")  metIdx=utils::cmssw::JESDOWN; 
	if(var == "umetup")   metIdx=utils::cmssw::UMETUP;
	if(var == "umetdown") metIdx=utils::cmssw::UMETDOWN;
	LorentzVector met=metVars[metIdx];

	data::PhysicsObjectCollection_t looseJets,selJets,selbJets;
	for(size_t ijet=0; ijet<jets.size(); ijet++)
	  {
	    if(var == "jerup")   jets[ijet].setVal("pt", jets[ijet].getVal("jerup")   );
	    if(var == "jerdown") jets[ijet].setVal("pt", jets[ijet].getVal("jerdown") );
	    if(var == "jesup")   jets[ijet].setVal("pt", jets[ijet].getVal("jesup")   );
	    if(var == "jesdown") jets[ijet].setVal("pt", jets[ijet].getVal("jesdown") );
	    
	    //cross-clean with selected leptons 
	    double minDRlj(9999.);
	    for(size_t ilep=0; ilep<selLeptons.size(); ilep++)
	      minDRlj = TMath::Min( minDRlj, deltaR(jets[ijet],selLeptons[ilep]) );
	    if(minDRlj<0.4) continue;
	    
	    //require to pass the loose id
	    Int_t idbits=jets[ijet].get("idbits");
	    bool passPFloose( ((idbits>>0) & 0x1));
	    if(!passPFloose) continue;

	    //top candidate jets
	    looseJets.push_back(jets[ijet]);
	    if(jets[ijet].pt()<30 || fabs(jets[ijet].eta())>2.5 ) continue;
	    selJets.push_back(jets[ijet]);
	    //if(jets[ijet].getVal("supercsv") <= 0.531) continue;
	    if(jets[ijet].getVal("csv") <= 0.679) continue;
	    selbJets.push_back(jets[ijet]);
	  }
	sort(looseJets.begin(),looseJets.end(),data::PhysicsObject_t::sortByPt);
	sort(selJets.begin(),  selJets.end(),  data::PhysicsObject_t::sortByCSV);
	sort(selbJets.begin(),  selbJets.end(),  data::PhysicsObject_t::sortByCSV);
	
	//select the event
	if(selLeptons.size()<2) continue;
	controlHistos.fillHisto("evtflow"+var, ch, 0, weight);
	controlHistos.fillHisto("nvertices",  ch, ev.nvtx, weight);
	
	LorentzVector ll=selLeptons[0]+selLeptons[1];
	float mll=ll.mass();
	float thetall=utils::cmssw::getArcCos<LorentzVector>(selLeptons[0],selLeptons[1]);
	float mtsum=utils::cmssw::getMT<LorentzVector>(selLeptons[0],met)+utils::cmssw::getMT<LorentzVector>(selLeptons[1],met);
	bool isZcand( isSameFlavor && fabs(mll-91)<15);
	//bool isOS( selLeptons[0].get("id")*selLeptons[1].get("id") < 0 ); 
	bool passDilSelection(mll>12 && !isZcand);
	bool passJetSelection(selJets.size()>=2);
	bool passMetSelection(met.pt()>40);//!isSameFlavor || met[0].pt()>40); (charged Higgs case expects high MET 
	
	//control distributions
	std::vector<TString> ctrlCategs;
	if(isOS && passDilSelection && passJetSelection  && passMetSelection)   ctrlCategs.push_back("");
//	if(isOS && passDilSelection && selJets.size()==1 && passMetSelection)   ctrlCategs.push_back("eq1jets");
//	if(isOS && passDilSelection && passJetSelection  && !passMetSelection)  ctrlCategs.push_back("lowmet");
//	if(isOS && passDilSelection && selJets.size()==1 && !passMetSelection)  ctrlCategs.push_back("eq1jetslowmet");
//	if(isOS && isZcand          && passJetSelection  && passMetSelection)   ctrlCategs.push_back("z");
//	if(isOS && isZcand          && selJets.size()==1 && passMetSelection)   ctrlCategs.push_back("zeq1jets");
//	if(isOS && isZcand          && passJetSelection  && !passMetSelection)  ctrlCategs.push_back("zlowmet");
//	if(isOS && isZcand          && selJets.size()==1 && !passMetSelection)  ctrlCategs.push_back("zeq1jetslowmet");
	for(size_t icat=0; icat<ctrlCategs.size(); icat++)
	  {
	    double ptmin(999999999);
	    double ht(0), htb(0), htnol(0), htbnol(0);
	    for(size_t ilep=0; ilep<2; ilep++)
	      {
		
		ht+= selLeptons[ilep].pt();
		htb+= selLeptons[ilep].pt();
		if(selLeptons[ilep].pt() < ptmin)
		  ptmin = selLeptons[ilep].pt();
		if(abs(selLeptons[ilep].get("id"))==11){
		  controlHistos.fillHisto(ctrlCategs[icat]+"emva", ch, selLeptons[ilep].getVal("mvatrig"), weight);
		  controlHistos.fillHisto(ctrlCategs[icat]+"pte",  ch, selLeptons[ilep].pt(),        weight);
		}
		else if(abs(selLeptons[ilep].get("id"))==13)
		  controlHistos.fillHisto(ctrlCategs[icat]+"ptmu",  ch, selLeptons[ilep].pt(),        weight);
	      }
	    for(size_t ijet=0; ijet<selJets.size(); ++ijet)
	      {
		ht+=selJets[ijet].pt();
		htnol+=selJets[ijet].pt();
		if(ijet<selbJets.size())
		  {
		    htb+=selbJets[ijet].pt();
		    htbnol+=selbJets[ijet].pt();
		  }
	      }
	    ht+=met.pt();
	    htb+=met.pt();
	    htnol+=met.pt();
	    htbnol+=met.pt();
	    
	    controlHistos.fillHisto(ctrlCategs[icat]+"ptmin",        ch, ptmin,           weight);
	    controlHistos.fillHisto(ctrlCategs[icat]+"mll",          ch, mll,             weight);
	    controlHistos.fillHisto(ctrlCategs[icat]+"ptll",         ch, ll.pt(),         weight);
	    controlHistos.fillHisto(ctrlCategs[icat]+"mtsum",        ch, mtsum,           weight);
	    controlHistos.fillHisto(ctrlCategs[icat]+"dilarccosine", ch, thetall,         weight);
	    controlHistos.fillHisto(ctrlCategs[icat]+"met",          ch, met.pt(),     weight);
	    controlHistos.fillHisto(ctrlCategs[icat]+"njets",        ch, selJets.size(),  weight);
	    controlHistos.fillHisto(ctrlCategs[icat]+"nbjets",       ch, selbJets.size(), weight);
	    controlHistos.fillHisto(ctrlCategs[icat]+"ht",           ch, ht,              weight);
	    controlHistos.fillHisto(ctrlCategs[icat]+"htb",          ch, htb,             weight);
	    controlHistos.fillHisto(ctrlCategs[icat]+"htnol",        ch, htnol,           weight);
	    controlHistos.fillHisto(ctrlCategs[icat]+"htbnol",       ch, htbnol,          weight);
	    
	    for(size_t ijet=0; ijet<looseJets.size(); ijet++)
	      {
		if(looseJets[ijet].pt()<30 || fabs(looseJets[ijet].eta())>2.5) continue;
		TString label("jet"); label+=(ijet+1);
		const data::PhysicsObject_t &genJet=looseJets[ijet].getObject("genJet");
		int flavId=genJet.info.find("id")->second;
		if(abs(flavId)==5 || abs(flavId)==4 ) flavId=abs(flavId)-1;
		else if(abs(flavId)>6)                flavId=1;
		else if(abs(flavId)==0)               flavId=0;
		else                                  flavId=2;
		controlHistos.fillHisto(ctrlCategs[icat]+"pt"+label+"pt",        ch, looseJets[ijet].pt(), weight);
		controlHistos.fillHisto(ctrlCategs[icat]+"pt"+label+"eta",       ch, fabs(looseJets[ijet].eta()), weight);
		controlHistos.fillHisto(ctrlCategs[icat]+"pt"+label+"flav",      ch, abs(flavId), weight);
		controlHistos.fillHisto(ctrlCategs[icat]+"pt"+label+"nobsmearpt",ch, abs(flavId)==5 ? looseJets[ijet].pt() : looseJets[ijet].getVal("jer"), weight);
		controlHistos.fillHisto(ctrlCategs[icat]+"pt"+label+"smearpt",   ch,                                         looseJets[ijet].getVal("jer"), weight);
	      }
	    
	    for(size_t ijet=0; ijet<selJets.size(); ijet++)
	      {
		TString label("jet"); label+=(ijet+1);
		const data::PhysicsObject_t &genJet=selJets[ijet].getObject("genJet");
		int flavId=genJet.info.find("id")->second;
		if(abs(flavId)==5 || abs(flavId)==4 ) flavId=abs(flavId)-1;
		else if(abs(flavId)>6)                flavId=1;
		else if(abs(flavId)==0)               flavId=0;
		else                                  flavId=2;
		controlHistos.fillHisto(ctrlCategs[icat]+"btag"+label+"pt",        ch, selJets[ijet].pt(), weight);
		controlHistos.fillHisto(ctrlCategs[icat]+"btag"+label+"eta",       ch, fabs(selJets[ijet].eta()), weight);
		controlHistos.fillHisto(ctrlCategs[icat]+"btag"+label+"flav",      ch, abs(flavId), weight);
		controlHistos.fillHisto(ctrlCategs[icat]+"btag"+label+"nobsmearpt",ch, abs(flavId)==5 ? selJets[ijet].pt() : selJets[ijet].getVal("jer"), weight);
		controlHistos.fillHisto(ctrlCategs[icat]+"btag"+label+"smearpt",   ch,                                       selJets[ijet].getVal("jer"), weight);
	      }
	  }

	//      if(passDilSelection &&                     passMetSelection && isOS) btvAn.analyze(selLeptons,looseJets,isMC,ev.nvtx,weight,weightUp,weightDown);
	//	if(passDilSelection && passJetSelection &&                     isOS) lxyAn.analyze(selLeptons,selJets,met[0],gen,weight);
	
	//select the event
	if(!passDilSelection) continue;
	controlHistos.fillHisto("evtflow"+var, ch, 1, weight);
	
	if(!passJetSelection) continue;
	controlHistos.fillHisto("evtflow"+var, ch, 2, weight);
	
	if(!passMetSelection) continue;
	controlHistos.fillHisto("evtflow"+var, ch, 3, weight);
	
	if(!isOS) continue;
	controlHistos.fillHisto("evtflow"+var, ch, 4, weight);
	
	
	//run the lxy analysis
	//	lxyAn.analyze(selLeptons,selJets,met[0],gen,weight);
	
	float nbtags(0);
	//	for(size_t ijet=0; ijet<selJets.size(); ijet++) nbtags += (selJets[ijet].getVal("supercsv")>0.531);
	for(size_t ijet=0; ijet<selJets.size(); ijet++) nbtags += (selJets[ijet].getVal("csv")>0.679);
	if(nbtags<2) continue;
	controlHistos.fillHisto("evtflow"+var, ch, 5, weight);
	
	if(spyEvents){
	  spyEvents->getEvent().cat=ev.cat;//dilId;
	  evSummaryWeight=xsecWeight*weight;
	  spyEvents->getTree()->Fill();
	}
	
	//PF candidates
	//data::PhysicsObjectCollection_t pf = evSummary.getPhysicsObject(DataEventSummaryHandler::PFCANDIDATES);
	//ueAn.analyze(selLeptons,selJets,met,pf,gen,weight);
      }
    }
  
  if(nDuplicates) cout << "[Warning] found " << nDuplicates << " duplicate events in this ntuple" << endl;
  
  //
  // close opened files
  // 
  inF->Close();
  if(spyFile){
    spyDir->cd(); spyEvents->getTree()->Write();
    spyFile->Close();
  }
  
  
  //
  // save histos to local file
  //
  TString outUrl(out);
  gSystem->ExpandPathName(outUrl);
  gSystem->Exec("mkdir -p " + outUrl);
  outUrl += "/";
  outUrl += proctag;
  if(mcTruthMode!=0) { outUrl += "_filt"; outUrl += mcTruthMode; }
  outUrl += ".root";
  TFile *file=TFile::Open(outUrl, "recreate");
  controlHistos.Write();
  file->Close();
  
  //that's all folks!
}  
