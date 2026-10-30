#include "rpiezo_solid_recipe.h"
#include "rpiezo_experiment_service.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace {
void require(bool value,const char *message){if(!value)throw std::invalid_argument(message);}
double positive(const QJsonObject &o,const char *key){auto v=o[key];require(v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()>0,"Solid template requires finite positive dimensions");return v.toDouble();}
}
namespace RPiezoSolidRecipe {
bool supports(const RModel &m){
    if(!m.getNElements())return false;
    for(uint i=0;i<m.getNElements();++i){auto t=m.getElement(i).getType();if(t!=R_ELEMENT_TETRA1&&t!=R_ELEMENT_HEXA1)return false;}
    return true;
}
RModel templateCandidate(const RModel &source,const QJsonObject &a){
    require(!source.getNNodes()&&!source.getNElements(),"Solid templates require an empty model");
    const auto kind=a["kind"].toString();require(kind=="solidHex"||kind=="solidTet","Unknown solid template");
    for(auto it=a.begin();it!=a.end();++it)require(QStringList{"kind","length","width","thickness","patchThickness","elements"}.contains(it.key()),"Unknown solid template field");
    const double length=positive(a,"length"),width=positive(a,"width"),thickness=positive(a,"thickness");
    const double patch=a.contains("patchThickness")?positive(a,"patchThickness"):thickness*.05;
    const double count=a["elements"].toDouble();require(a["elements"].isDouble()&&count>=1&&count<=32&&std::floor(count)==count,"Solid template requires 1..32 length cells");
    const uint nx=uint(count);RModel m=source;
    const std::array<double,3> zs{{0.,thickness,thickness+patch}};
    const auto node=[](uint x,uint y,uint z){return 6*x+2*z+y;};
    for(uint x=0;x<=nx;++x)for(uint z=0;z<3;++z)for(uint y=0;y<2;++y)m.addNode(RNode(length*x/nx,width*y,zs[z]));
    for(uint x=0;x<nx;++x)for(uint z=0;z<2;++z){
        const std::array<uint,8> ids{{node(x,0,z),node(x+1,0,z),node(x+1,1,z),node(x,1,z),node(x,0,z+1),node(x+1,0,z+1),node(x+1,1,z+1),node(x,1,z+1)}};
        if(kind=="solidHex"){RElement e(R_ELEMENT_HEXA1);for(uint j=0;j<8;++j)e.setNodeId(j,ids[j]);m.addElement(e,true,0);}
        else {constexpr uint tets[6][4]={{0,1,2,6},{0,2,3,6},{0,3,7,6},{0,7,4,6},{0,4,5,6},{0,5,1,6}};
            for(const auto &tet:tets){RElement e(R_ELEMENT_TETRA1);for(uint j=0;j<4;++j)e.setNodeId(j,ids[tet[j]]);m.addElement(e,true,0);}}
    }
    // RMaterial creates a random UUID by default. A generated template is
    // rebuilt during preview confirmation, so its empty host material identity
    // must be reproducible across those equivalent candidate compilations.
    const QUuid materialNamespace(QStringLiteral("{8a77ab93-7261-5a59-a10f-0f218b8a7505}"));
    for(uint i=0;i<m.getNVolumes();++i){
        const QString identity=QStringLiteral("range-fea/piezo-solid-template/v1/%1/volume/%2")
            .arg(kind).arg(m.getVolume(i).getPhysicalId());
        m.getVolume(i).getMaterial().setID(QUuid::createUuidV5(materialNamespace,identity.toUtf8()));
    }
    return m;
}
QJsonObject example(const RModel &source,const QJsonArray &selection){
    require(supports(source),"Solid control requires TETRA1/HEXA1 volume elements");
    // Reuse the shared documented demonstration materials, never host legacy data.
    auto recipe=RPiezoExperimentService::exampleRecipe(RModel{},"planarBeam");require(!recipe.isEmpty(),"Cannot construct shared SI demonstration defaults");
    recipe.remove("template");recipe["family"]="solid3D";
    recipe["frame"]=QJsonObject{{"origin",QJsonArray{0.,0.,0.}},{"ex",QJsonArray{1.,0.,0.}},{"ey",QJsonArray{0.,1.,0.}},{"ez",QJsonArray{0.,0.,1.}}};
    std::set<uint> selected,actual;for(uint i=0;i<source.getNElements();++i)actual.insert(source.getElementPhysicalId(i));
    for(const auto &v:selection){const double x=v.toDouble();require(v.isDouble()&&x>=1&&x<=std::numeric_limits<uint>::max()&&std::floor(x)==x,"Invalid selected piezo volume ID");require(selected.insert(uint(x)).second&&actual.count(uint(x)),"Duplicate or unknown piezo volume ID");}
    double minX=std::numeric_limits<double>::infinity(),maxX=-minX,maxZ=-minX;uint tip=0;
    for(uint i=0;i<source.getNNodes();++i){const auto &n=source.getNode(i);minX=std::min(minX,n.getX());if(n.getX()>maxX){maxX=n.getX();tip=i;}maxZ=std::max(maxZ,n.getZ());}
    require(maxX>minX,"Solid example requires nonzero global X extent");
    // With no selection, use only the uppermost cell layer. Existing meshes
    // require separately meshed patch volumes; this never invents surface ties.
    if(selected.empty())for(uint i=0;i<source.getNElements();++i){const auto &e=source.getElement(i);bool top=false;for(uint j=0;j<e.size();++j)top=top||std::abs(source.getNode(e.getNodeId(j)).getZ()-maxZ)<=1e-10*std::max(1.,std::abs(maxZ));if(top)selected.insert(source.getElementPhysicalId(i));}
    double lo=std::numeric_limits<double>::infinity(),hi=-lo;
    for(uint i=0;i<source.getNElements();++i)if(selected.count(source.getElementPhysicalId(i))){const auto &e=source.getElement(i);for(uint j=0;j<e.size();++j){const double z=source.getNode(e.getNodeId(j)).getZ();lo=std::min(lo,z);hi=std::max(hi,z);}}
    require(hi>lo,"Selected piezo volumes require distinct electrode planes");
    QJsonArray members;
    for(uint i=0;i<source.getNElements();++i){const uint id=source.getElementPhysicalId(i);const bool active=selected.count(id)!=0;
        QJsonObject layer{{"materialId",active?"examplePiezo":"host"},{"lowerZ",active?lo:0.},{"upperZ",active?hi:1.},{"width",1.},{"portId",active?"drive":""},{"polarity",1},{"coverage",QJsonArray{-1.,1.,-1.,1.}}};
        members.append(QJsonObject{{"physicalElementId",double(id)},{"layers",QJsonArray{layer}}});
    }
    recipe["members"]=members;recipe["ports"]=QJsonArray{QJsonObject{{"portId","drive"},{"role","actuator"},{"readout","actuatorCharge"},{"initialValue",0.}}};
    QJsonArray supports;for(uint i=0;i<source.getNNodes();++i)if(std::abs(source.getNode(i).getX()-minX)<=1e-10*std::max(1.,maxX-minX))for(const char *dof:{"UX","UY","UZ"})supports.append(QJsonObject{{"physicalNodeId",double(source.getNodePhysicalId(i))},{"dof",dof},{"value",0.}});
    recipe["supports"]=supports;
    QJsonArray outputs;const char *quantities[]={"displacement","velocity","acceleration"},*units[]={"m","m/s","m/s^2"};for(int i=0;i<3;++i)outputs.append(QJsonObject{{"id",QString("tip_%1").arg(quantities[i])},{"physicalNodeId",double(source.getNodePhysicalId(tip))},{"dof","Z"},{"quantity",quantities[i]},{"unit",units[i]}});
    recipe["design"]=QJsonObject{{"outputs",outputs}};recipe["replaceLegacyConditions"]=false;
    return recipe;
}
}
