{%MainUnit castleifc.pas}
{
  Copyright 2024-2024 Michalis Kamburelis.

  This file is part of "Castle Game Engine".

  "Castle Game Engine" is free software; see the file COPYING.txt,
  included in this distribution, for details about the copyright.

  "Castle Game Engine" is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

  ----------------------------------------------------------------------------
}

{ IFC conversion to X3D. }

{$ifdef read_interface_types}

  { Maintains mapping between IFC concepts and X3D nodes.
    Allows to comfortably load IFC file and convert it to X3D multple
    times (using @link(Load), @link(Update), @link(RootNode)).
    Allows also to resolve X3D nodes back to IFC concepts
    (using @link(NodeToProduct)). }
  TCastleIfcMapping = class
  strict private
    FBaseUrl: String;
    FRootNode: TX3DRootNode;

    { Use ObjectPlacementToParentNodes and ChildrenNodesToObjectPlacement
      to move the nodes (change parents) to reflect
      TIfcObjectPlacement.PlacementRelTo instructions. }
    procedure FixParentChildRelationships;

    function CreateRootNode(const Ifc: TIfcFile): TX3DRootNode;
  private
    { Within the CastleIfc unit, this class is a container for information
      and actions useful to all
      node-building routines that build/update X3D stuff from IFC.
      The things below are used by BuildNode methods of IFC classes. }

    type
      TObjectPlacementToParentNodes =
        {$ifdef FPC}specialize{$endif} TDictionary<TIfcObjectPlacement, TTransformNode>;
      TChildrenNodesToObjectPlacement =
        {$ifdef FPC}specialize{$endif} TDictionary<TTransformNode, TIfcObjectPlacement>;

    var
      { Map each encountered TIfcObjectPlacement to an X3D TTransformNode. }
      ObjectPlacementToParentNodes: TObjectPlacementToParentNodes;

      { Map each children node (TTransformNode) to a desired parent,
        expressed as TIfcObjectPlacement.
        Together, ObjectPlacementToParentNodes and ChildrenNodesToObjectPlacement
        allow to set the visual transformation (parent-child relationships
        between X3D nodes) to follow IFC TIfcObjectPlacement.PlacementRelTo idea. }
      ChildrenNodesToObjectPlacement: TChildrenNodesToObjectPlacement;

      { Appearance of the current object definition being built.
        TODO: keeping one current appearance like this is a simplification,
        materials should be passed down to product children and only to them. }
      CurrentAppearance: TAppearanceNode;

    { BaseUrl should be passed to all X3D nodes created by this class,
      so they have TX3DRootNode.BaseUrl equal to this. }
    property BaseUrl: String read FBaseUrl;
  public
    { Load IFC instance (TIfcFile) converting it to a new X3D node
      (available in @link(RootNode)).

      The BaseUrl will be used to resolve relative URLs,
      e.g. texture URLs mentioned in IFC information.

      After calling this (and before destroying this class or calling
      @link(Load) again) remember to free the resulting @link(RootNode),
      it is not freed automatically by this class (because in all practical
      use-cases, you want to keep the X3D node around,
      e.g. to be owned by TCastleScene or freed manually later). }
    procedure Load(const Ifc: TIfcFile; const ABaseUrl: String);

    { Latest X3D root node, built from the latest IFC instance. }
    property RootNode: TX3DRootNode read FRootNode;

    { Update the nodes in @link(RootNode) based on the new IFC contents.
      Call this only after calling @link(Load) once.

      Tries to be efficient and do as little work as possible
      and update X3D graph in @link(RootNode) to reflect the new IFC contents.
      For example, changing a translation in IFC will just change the corresponding
      @link(TTransformNode.Translation), without recreating any X3D nodes. }
    procedure Update(const Ifc: TIfcFile);

    { For a shape node (TShapeNode), return the corresponding IFC product
      (TIfcProduct descendant, like a TIfcWall, TIfcWindow, TIfcDoor).
      Returns @nil if cannot be determined.
      Assumes that the nodes have been updated to reflect the latest state
      of the IFC classes using @link(Update). }
    function NodeToProduct(const Node: TShapeNode): TIfcProduct;
  end;

{$endif read_interface_types}

{$ifdef read_interface}

{ Convert IFC file to X3D. }
function IfcToX3D(const Ifc: TIfcFile; const BaseUrl: String): TX3DRootNode;

{ Convert IFC file to X3D. }
function X3DToIfc(const RootNode: TX3DRootNode): TIfcFile;

{$endif read_interface}

{$ifdef read_implementation}

{ Set metadata of a node. }
procedure SetMetadata(const Node: TAbstractNode; const Key, Value: String);
begin
  if Value <> '' then // do not set empty (default in IFC) values, to keep X3D output smaller
  begin
    if Node is TX3DRootNode then
      { TODO: "metadata" of TX3DRootNode is not serialized, but META is.
        Yeah, there are effectively two ways to store metadata in X3D. }
      TX3DRootNode(Node).Meta[Key] := Value
    else
      Node.MetadataString[Key] := Value;
  end;
end;

{ TCastleIfcMapping ---------------------------------------------------------- }

procedure TCastleIfcMapping.FixParentChildRelationships;

  { Go up the nodes hierachy, returning direct parent (any TAbstractGroupingNode)
    and possibly non-direct TransformParent (TTransformNode) parent.
    Returns @true and for sure non-nil DirectParent, TransformParent on success.
    On failure returns @false and DirectParent, TransformParent are undefined.

    This way of searching for 2 parents accounts for the fact that
    things in TIfcProduct are wrapped in additional groups related to 'Representation'.
    We don't need to move things out of these groups,
    if ultimately they are placed in the right TTransformNode. }
  function GetParentNode(const Node: TTransformNode;
    out DirectParent: TAbstractGroupingNode; out TransformParent: TTransformNode): Boolean;

    function GetDirectParent(const Node: TAbstractGroupingNode): TAbstractGroupingNode;
    begin
      Result := nil;
      if Node.ParentFieldsCount > 1 then
      begin
        WritelnWarning('IFC', 'Corresponding X3D node has more than one parent field, cannot fix parents to follow placementRelTo', [
          Node.NiceName
        ]);
      end else
      if Node.ParentFieldsCount = 1 then
      begin
        if Node.ParentFields[0].ParentNode is TAbstractGroupingNode then
        begin
          Result := TAbstractGroupingNode(Node.ParentFields[0].ParentNode)
        end else
        begin
          WritelnWarning('IFC', 'Corresponding X3D node has parent field that is not a TAbstractGroupingNode, cannot fix parents to follow placementRelTo', [
            Node.NiceName
          ]);
        end;
      end;
    end;

  var
    PotentialTransformParent: TAbstractGroupingNode;
  begin
    Result := false;
    DirectParent := GetDirectParent(Node);
    TransformParent := nil; // initialize to nil
    if DirectParent = nil then
      Exit; // exit with Result = false

    PotentialTransformParent := DirectParent;
    while not (PotentialTransformParent is TTransformNode) do
    begin
      PotentialTransformParent := GetDirectParent(PotentialTransformParent);
      if PotentialTransformParent = nil then
        Exit; // exit with Result = false
    end;
    Assert(PotentialTransformParent is TTransformNode);
    TransformParent := TTransformNode(PotentialTransformParent);
    Result := true;
  end;

var
  ChildToPlacement:
    {$ifdef FPC}TChildrenNodesToObjectPlacement.TDictionaryPair
    {$else}TPair<TTransformNode, TIfcObjectPlacement>
    {$endif};
  Child, OldParentTransform, NewParent: TTransformNode;
  OldParentDirect: TAbstractGroupingNode;
begin
  for ChildToPlacement in ChildrenNodesToObjectPlacement do
  begin
    Child := ChildToPlacement.Key;
    if GetParentNode(Child, OldParentDirect, OldParentTransform) then
    begin
      if ObjectPlacementToParentNodes.TryGetValue(ChildToPlacement.Value, NewParent) and
         (OldParentTransform <> NewParent) then
      begin
        WritelnLog('IFC', 'Fixing parent of "%s" to be relative to ObjectPlacement GUID "%s" to follow placementRelTo', [
          Child.NiceName,
          ChildToPlacement.Value.GlobalId
        ]);
        Child.KeepExistingBegin; // do not free Child because ref count temporarily drops to zero below
        OldParentDirect.RemoveChildren(Child);
        NewParent.AddChildren(Child);
        Child.KeepExistingEnd;
      end;
    end;
  end;
end;

function TCastleIfcMapping.CreateRootNode(const Ifc: TIfcFile): TX3DRootNode;
begin
  { Initialize things used only temporarily during Load. }
  ObjectPlacementToParentNodes := TObjectPlacementToParentNodes.Create;
  ChildrenNodesToObjectPlacement := TChildrenNodesToObjectPlacement.Create;
  try
    Result := Ifc.BuildNode(Self);
    FixParentChildRelationships;
  finally
    FreeAndNil(ObjectPlacementToParentNodes);
    FreeAndNil(ChildrenNodesToObjectPlacement);
    FreeIfUnusedAndNil(CurrentAppearance);
  end;
end;

procedure TCastleIfcMapping.Load(const Ifc: TIfcFile; const ABaseUrl: String);
begin
  { Node that we don't free previous FRootNode, we don't own it. }
  FBaseUrl := ABaseUrl;
  FRootNode := CreateRootNode(Ifc);
end;

procedure TCastleIfcMapping.Update(const Ifc: TIfcFile);
var
  NewRootNode: TX3DRootNode;
  ChildNode: TAbstractChildNode;
  I: Integer;
begin
  // TODO: very inefficient implementation that removes old, adds new
  NewRootNode := CreateRootNode(Ifc);
  try
    FRootNode.ClearChildren;
    for I := 0 to NewRootNode.FdChildren.Count - 1 do
    begin
      ChildNode := NewRootNode.FdChildren[I] as TAbstractChildNode;
      FRootNode.AddChildren(ChildNode);
    end;
  finally FreeAndNil(NewRootNode) end;
end;

function TCastleIfcMapping.NodeToProduct(const Node: TShapeNode): TIfcProduct;
begin
  // TODO
  Result := nil;
end;

{ BuildNode methods implementations ----------------------------------------- }

function TIfcFile.BuildNode(const Mapping: TCastleIfcMapping): TX3DRootNode;
begin
  Result := TX3DRootNode.Create('', Mapping.BaseUrl);
  SetMetadata(Result, 'IFC_Version', Version);
  SetMetadata(Result, 'IFC_SchemaIdentifier', SchemaIdentifier);
  SetMetadata(Result, 'IFC_OriginatingSystem', OriginatingSystem);
  SetMetadata(Result, 'IFC_PreprocessorVersion', PreprocessorVersion);
  SetMetadata(Result, 'IFC_TimeStamp', TimeStamp);

  Result.AddChildren(Project.BuildNode(Mapping));
end;

function TIfcRepresentationItemList.BuildNode(const Mapping: TCastleIfcMapping): TAbstractChildNode;
var
  RepresentationItem: TIfcRepresentationItem;
  Group: TGroupNode;
  Child: TAbstractChildNode;
begin
  Group := TGroupNode.Create('', Mapping.BaseUrl);
  SetMetadata(Group, 'IFC_ListOfChildren', ClassOfListChildren(Self).ClassName);
  Result := Group;

  for RepresentationItem in Self do
  begin
    Child := RepresentationItem.BuildNode(Mapping);
    if Child <> nil then
      Group.AddChildren(Child);
  end;
end;

function TIfcRepresentationList.BuildNode(const Mapping: TCastleIfcMapping): TAbstractChildNode;
var
  Representation: TIfcRepresentation;
  Group: TGroupNode;
begin
  Group := TGroupNode.Create('', Mapping.BaseUrl);
  SetMetadata(Group, 'IFC_ListOfChildren', ClassOfListChildren(Self).ClassName);
  Result := Group;

  for Representation in Self do
    Group.AddChildren(Representation.Items.BuildNode(Mapping));
end;

function TIfcProductRepresentation.BuildNode(const Mapping: TCastleIfcMapping): TAbstractChildNode;
var
  Group: TGroupNode;
begin
  Group := TGroupNode.Create('', Mapping.BaseUrl);
  SetMetadata(Group, 'IFC_ClassName', ClassName);
  SetMetadata(Group, 'IFC_Name', Name);
  SetMetadata(Group, 'IFC_Description', Description);
  Result := Group;

  Group.AddChildren(Representations.BuildNode(Mapping));
end;

function TIfcRelAggregates.BuildNode(const Mapping: TCastleIfcMapping): TAbstractChildNode;
var
  RelatedObject: TIfcObjectDefinition;
  Group: TGroupNode;
begin
  Group := TGroupNode.Create('', Mapping.BaseUrl);
  SetMetadata(Group, 'IFC_ClassName', ClassName);
  SetMetadata(Group, 'IFC_Name', Name);
  SetMetadata(Group, 'IFC_Description', Description);
  Result := Group;

  for RelatedObject in RelatedObjects do
    Group.AddChildren(RelatedObject.BuildNode(Mapping));
end;

function TIfcRelContainedInSpatialStructure.BuildNode(const Mapping: TCastleIfcMapping): TAbstractChildNode;
var
  Product: TIfcProduct;
  Group: TGroupNode;
begin
  Group := TGroupNode.Create('', Mapping.BaseUrl);
  SetMetadata(Group, 'IFC_ClassName', ClassName);
  SetMetadata(Group, 'IFC_Name', Name);
  SetMetadata(Group, 'IFC_Description', Description);
  Result := Group;

  for Product in RelatedElements do
    Group.AddChildren(Product.BuildNode(Mapping));
end;

function TIfcRelVoidsElement.BuildNode(const Mapping: TCastleIfcMapping): TAbstractChildNode;
var
  Group: TGroupNode;
begin
  Group := TGroupNode.Create('', Mapping.BaseUrl);
  SetMetadata(Group, 'IFC_ClassName', ClassName);
  Result := Group;

  if RelatedOpeningElement <> nil then
    Group.AddChildren(RelatedOpeningElement.BuildNode(Mapping));
end;

procedure TIfcCartesianPointList2D.UpdateCoordinate(const Coord: TCoordinateNode);
var
  I: Integer;
begin
  Coord.FdPoint.Count := CoordList.Count;
  for I := 0 to CoordList.Count - 1 do
    Coord.FdPoint.Items[I] := Vector3(CoordList[I], 0);
end;

procedure TIfcCartesianPointList3D.UpdateCoordinate(const Coord: TCoordinateNode);
// var
//   I: Integer;
begin
  Coord.FdPoint.Items.Assign(CoordList);
  // Coord.FdPoint.Count := CoordList.Count;
  // for I := 0 to CoordList.Count - 1 do
  //   Coord.FdPoint.Items[I] := CoordList[I];
end;

{ UpdateCrossSection method overrides ---------------------------------------- }

procedure TIfcCurve.UpdateCrossSection(const Extrusion: TExtrusionNode);
var
  CrossSection: TVector2List;

  procedure HandleIndexedLineSetNode(const IndexedLineSet: TIndexedLineSetNode);
  var
    Coord: TCoordinateNode;
    I, Index: Integer;
  begin
    Coord := IndexedLineSet.Coord as TCoordinateNode;
    Assert(Coord <> nil); // these X3D nodes are build by us, so we know they have Coord
    for I := 0 to IndexedLineSet.FdCoordIndex.Count - 1 do
    begin
      Index := IndexedLineSet.FdCoordIndex.Items[I];
      if Index < 0 then
      begin
        WritelnWarning('IFC', 'TODO: Unsupported IfcIndexedPolyCurve for extrusion cross section (we need to split into multiple X3D Extrusions to handle disjoint sections)');
      end else
      begin
        Assert(Index < Coord.FdPoint.Count);
        CrossSection.Add(Coord.FdPoint.Items[Index].XY);
      end;
    end;
  end;

  procedure HandleLineSetNode(const LineSet: TLineSetNode);
  var
    Coord: TCoordinateNode;
    I: Integer;
  begin
    Coord := LineSet.Coord as TCoordinateNode;
    for I := 0 to Coord.FdPoint.Count - 1 do
      CrossSection.Add(Coord.FdPoint.Items[I].XY);
  end;

var
  Shape: TShapeNode;
  Mapping: TCastleIfcMapping;
begin
  CrossSection := TVector2List.Create;
  try
    Mapping := TCastleIfcMapping.Create;
    try
      Shape := BuildNode(Mapping) as TShapeNode;
      try
        if Shape.Geometry <> nil then
        begin
          if Shape.Geometry is TIndexedLineSetNode then
            HandleIndexedLineSetNode(TIndexedLineSetNode(Shape.Geometry))
          else
          if Shape.Geometry is TLineSetNode then
            HandleLineSetNode(TLineSetNode(Shape.Geometry))
          else
          if Shape.Geometry <> nil then
            WritelnWarning('IFC', 'Unsupported Shape.Geometry class for TIfcCurve.UpdateCrossSection: ' + Shape.Geometry.ClassName);
        end;
      finally FreeIfUnusedAndNil(Shape) end;
    finally FreeAndNil(Mapping) end;

    // actually use our CrossSection contents, this is all we wanted to calculate
    Extrusion.SetCrossSection(CrossSection);
  finally FreeAndNil(CrossSection) end;
end;

procedure TIfcPolyline.UpdateCrossSection(const Extrusion: TExtrusionNode);
var
  CrossSection: TVector2List;
  I: Integer;
begin
  CrossSection := TVector2List.Create;
  try
    CrossSection.Count := Points.Count;
    for I := 0 to Points.Count - 1 do
      CrossSection[I] := Points[I].Coordinates.Value.XY;
    Extrusion.SetCrossSection(CrossSection);
  finally FreeAndNil(CrossSection) end;
end;

{ BuildNode of TIfcRepresentationItem and descendants ------------------------- }

function TIfcRepresentationItem.BuildNode(const Mapping: TCastleIfcMapping): TAbstractChildNode;
begin
  Result := nil;
end;

function TIfcPolyline.BuildNode(const Mapping: TCastleIfcMapping): TAbstractChildNode;
var
  Shape: TShapeNode;
  LineSet: TLineSetNode;
  Coord: TCoordinateNode;
  I: Integer;
begin
  Coord := TCoordinateNode.Create('', Mapping.BaseUrl);
  Coord.FdPoint.Count := Points.Count;
  for I := 0 to Points.Count - 1 do
    Coord.FdPoint.Items[I] := Points[I].Coordinates.Value;

  LineSet := TLineSetNode.Create('', Mapping.BaseUrl);
  LineSet.Coord := Coord;
  LineSet.SetVertexCount([Points.Count]);

  Shape := TShapeNode.Create('', Mapping.BaseUrl);
  Shape.Geometry := LineSet;
  // lines are best displayed as unlit geometry, different material would be useful
  // Shape.Appearance := Mapping.CurrentAppearance;
  SetMetadata(Shape, 'IFC_ClassName', ClassName);

  Result := Shape;
end;

function TIfcIndexedPolyCurve.BuildNode(const Mapping: TCastleIfcMapping): TAbstractChildNode;

  { Add integers for NewIndexes to Indexes, decrementing all values
    on NewIndexes by 1.
    Useful as IFC indices are 1-based, X3D is 0-based.
    So instead of

      IndexedLineSet.FdCoordIndex.Items.AddRange(LineIndex.Value);

    you must do

      AddRangeDecrement(IndexedLineSet.FdCoordIndex.Items, LineIndex.Value);
  }
  procedure AddRangeDecrement(const Indexes: TInt32List; const NewIndexes: TIntegerList);
  var
    I: Integer;
  begin
    for I := 0 to NewIndexes.Count - 1 do
      Indexes.Add(NewIndexes[I] - 1);
  end;

var
  Shape: TShapeNode;
  IndexedLineSet: TIndexedLineSetNode;
  LineSet: TLineSetNode;
  Coord: TCoordinateNode;
  Segment: TIfcSegmentIndexSelect;
  LineIndex: TIfcLineIndex;
  ArcIndex: TIfcArcIndex;
begin
  Coord := TCoordinateNode.Create('', Mapping.BaseUrl);
  Points.UpdateCoordinate(Coord);

  Shape := TShapeNode.Create('', Mapping.BaseUrl);
  // lines are best displayed as unlit geometry, different material would be useful
  // Shape.Appearance := Mapping.CurrentAppearance;
  SetMetadata(Shape, 'IFC_ClassName', ClassName);

  if Segments.Count = 0 then
  begin
    { IFC spec says that Segments are optional:
      "In the case that the list of Segments is not provided,
      all points in the IfcCartesianPointList are connected by straight
      line segments in the order they appear in the IfcCartesianPointList." }

    LineSet := TLineSetNode.Create('', Mapping.BaseUrl);
    LineSet.Coord := Coord;
    LineSet.SetVertexCount([Coord.FdPoint.Count]);
    Shape.Geometry := LineSet;
  end else
  begin
    IndexedLineSet := TIndexedLineSetNode.Create('', Mapping.BaseUrl);
    IndexedLineSet.Coord := Coord;
    IndexedLineSet.Mode := lmStrip;
    Shape.Geometry := IndexedLineSet;

    for Segment in Segments do
    begin
      if Segment is TIfcLineIndex then
      begin
        LineIndex := TIfcLineIndex(Segment);
        AddRangeDecrement(IndexedLineSet.FdCoordIndex.Items, LineIndex.Value);
        IndexedLineSet.FdCoordIndex.Items.Add(-1);
      end else
      if Segment is TIfcArcIndex then
      begin
        ArcIndex := TIfcArcIndex(Segment);
        AddRangeDecrement(IndexedLineSet.FdCoordIndex.Items, ArcIndex.Value);
        IndexedLineSet.FdCoordIndex.Items.Add(-1);
      end else
        WritelnWarning('IFC', 'Unsupported Segment class: ' + Segment.ClassName);
    end;
  end;

  Result := Shape;
end;

function TIfcExtrudedAreaSolid.BuildNode(const Mapping: TCastleIfcMapping): TAbstractChildNode;
var
  Extrusion: TExtrusionNode;
  SweptAreaProf: TIfcArbitraryClosedProfileDef;
  Shape: TShapeNode;
begin
  Extrusion := TExtrusionNode.Create('', Mapping.BaseUrl);
  Extrusion.SetSpine([
    TVector3.Zero,
    ExtrudedDirection.DirectionRatios.Value * Depth
  ]);
  if SweptArea is TIfcArbitraryClosedProfileDef then // also checks is it <> nil
  begin
    SweptAreaProf := TIfcArbitraryClosedProfileDef(SweptArea);
    SweptAreaProf.OuterCurve.UpdateCrossSection(Extrusion);
  end else
  if SweptArea <> nil then
    WritelnWarning('IFC', 'Unsupported IfcExtrudedAreaSolid.SweptArea class: ' + SweptArea.ClassName);

  Shape := TShapeNode.Create('', Mapping.BaseUrl);
  Shape.Geometry := Extrusion;
  Shape.Appearance := Mapping.CurrentAppearance;
  SetMetadata(Shape, 'IFC_ClassName', ClassName);

  Result := Shape;
end;

function TIfcPolygonalFaceSet.BuildNode(const Mapping: TCastleIfcMapping): TAbstractChildNode;
var
  IndexedFaceSet: TIndexedFaceSetNode;
  Coord: TCoordinateNode;
  Face: TIfcIndexedPolygonalFace;
  I, Index: Integer;
  Shape: TShapeNode;
begin
  Coord := TCoordinateNode.Create('', Mapping.BaseUrl);
  if Coordinates <> nil then
    Coord.SetPoint(Coordinates.CoordList);

  IndexedFaceSet := TIndexedFaceSetNode.Create('', Mapping.BaseUrl);
  IndexedFaceSet.Coord := Coord;

  for Face in Faces do
  begin
    for I := 0 to Face.CoordIndex.Count - 1 do
    begin
      Index := Face.CoordIndex[I];
      if Index < PnIndex.Count then
        Index := PnIndex[Index];
      Dec(Index); // IFC indices are 1-based
      IndexedFaceSet.FdCoordIndex.Items.Add(Index);
    end;
    IndexedFaceSet.FdCoordIndex.Items.Add(-1);
  end;

  Shape := TShapeNode.Create('', Mapping.BaseUrl);
  Shape.Geometry := IndexedFaceSet;
  Shape.Appearance := Mapping.CurrentAppearance;
  SetMetadata(Shape, 'IFC_ClassName', ClassName);

  Result := Shape;
end;

{ BuildNode of TIfcObjectDefinition and descendants -------------------------- }

function TIfcObjectDefinition.BuildNode(const Mapping: TCastleIfcMapping): TTransformNode;

  { Derive X3D TAppearanceNode from IFC TIfcObjectDefinition.HasAssociations. }
  function AppearanceFromAssociations(const Associations: TIfcRelAssociatesList): TAppearanceNode;
  begin
    (*
    for Rel in Associations do
    begin
      if Rel is TIfcRelAssociatesMaterial then
      begin
        RelMat := TIfcRelAssociatesMaterial(Rel);
        Result := TAppearanceNode.Create('', Mapping.BaseUrl);
        Result.Material := TPhysicalMaterialNode.Create('', Mapping.BaseUrl);
        // we would like to set Result.Material.BaseColor based on IFC information,
        // but it seems IFC materials don't specify simple color
        Exit;
      end;
    *)

    Result := TAppearanceNode.Create('', Mapping.BaseUrl);
    //Result.Material := TPhysicalMaterialNode.Create('', Mapping.BaseUrl);
    // Phong material looks better alone, with default color and lighting
    Result.Material := TMaterialNode.Create('', Mapping.BaseUrl);
  end;

var
  RelAggregates: TIfcRelAggregates;
begin
  Result := TTransformNode.Create('', Mapping.BaseUrl);
  SetMetadata(Result, 'IFC_ClassName', ClassName);
  SetMetadata(Result, 'IFC_Name', Name);
  SetMetadata(Result, 'IFC_Description', Description);

  // Things inside IsDecomposedBy are affected by given material
  FreeIfUnusedAndNil(Mapping.CurrentAppearance);
  Mapping.CurrentAppearance := AppearanceFromAssociations(HasAssociations);

  for RelAggregates in IsDecomposedBy do
    Result.AddChildren(RelAggregates.BuildNode(Mapping));
end;

function TIfcSpatialElement.BuildNode(const Mapping: TCastleIfcMapping): TTransformNode;
var
  RelContainedInSpatialStructure: TIfcRelContainedInSpatialStructure;
begin
  Result := inherited;
  for RelContainedInSpatialStructure in ContainsElements do
    Result.AddChildren(RelContainedInSpatialStructure.BuildNode(Mapping));
end;

function TIfcProduct.BuildNode(const Mapping: TCastleIfcMapping): TTransformNode;
begin
  Result := inherited;

  if ObjectPlacement <> nil then
    ObjectPlacement.UpdateNode(Mapping, Result);

  if Representation <> nil then
    Result.AddChildren(Representation.BuildNode(Mapping));
end;

function TIfcElement.BuildNode(const Mapping: TCastleIfcMapping): TTransformNode;
var
  RelVoidsElement: TIfcRelVoidsElement;
begin
  Result := inherited;
  SetMetadata(Result, 'IFC_Tag', Tag);

  // TODO: This is just for debug, the things in HasOpenings should actually subtract geometry
  for RelVoidsElement in HasOpenings do
  begin
    // debug: make material transparent if something has holes
    (Mapping.CurrentAppearance.Material as TMaterialNode).Transparency := 0.2;
    Result.AddChildren(RelVoidsElement.BuildNode(Mapping));
  end;
end;

function TIfcContext.BuildNode(const Mapping: TCastleIfcMapping): TTransformNode;
begin
  Result := inherited;
  SetMetadata(Result, 'IFC_ObjectType', ObjectType);
  SetMetadata(Result, 'IFC_LongName', LongName);
  SetMetadata(Result, 'IFC_Phase', Phase);
end;

{ UpdateNode methods implementations ----------------------------------------- }

procedure TIfcObjectPlacement.UpdateNode(const Mapping: TCastleIfcMapping;
  const Transform: TTransformNode);
begin
  // inherited; // no UpdateNode in ancestor

  Mapping.ObjectPlacementToParentNodes.AddOrSetValue(Self, Transform);
  if PlacementRelTo <> nil then
    Mapping.ChildrenNodesToObjectPlacement.AddOrSetValue(Transform, PlacementRelTo);
end;

procedure TIfcLocalPlacement.UpdateNode(const Mapping: TCastleIfcMapping;
  const Transform: TTransformNode);
begin
  inherited;
  if RelativePlacement <> nil then
    RelativePlacement.UpdateNode(Mapping, Transform);
end;

procedure TIfcPlacement.UpdateNode(const Mapping: TCastleIfcMapping;
  const Transform: TTransformNode);
begin
  // inherited; // no UpdateNode in ancestor

  if Location is TIfcCartesianPoint then
  begin
    Transform.Translation := TIfcCartesianPoint(Location).Coordinates.Value;
  end;
  // TODO: TIfcAxis2Placement2D/3D may define axis and ref direction, should cause rotation?
end;

{ IfcToX3D and back ---------------------------------------------------------- }

function IfcToX3D(const Ifc: TIfcFile; const BaseUrl: String): TX3DRootNode;
var
  Mapping: TCastleIfcMapping;
begin
  Mapping := TCastleIfcMapping.Create;
  try
    Mapping.Load(Ifc, BaseUrl);
    Result := Mapping.RootNode;
  finally FreeAndNil(Mapping) end;
end;

function X3DToIfc(const RootNode: TX3DRootNode): TIfcFile;
begin
  Result := TIfcFile.Create(nil);
  // TODO
end;

{$endif read_implementation}